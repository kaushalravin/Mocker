#!/bin/sh
#
# mockerhost's teardown: the reverse of clone_and_build.sh. Design and safety
# reasoning: docs/mockerhost-teardown-design.md.
#
# Usage: sudo sh mockerhost/teardown.sh <name>
#
# Operator-only on purpose (no HTTP endpoint): the control server has no
# authentication, and teardown destroys data.
#
# Order (reverse of deploy, so a failure at any step leaves a safe,
# re-runnable state - every step skips if its target is already gone):
#   1. remove the nginx location + reload     (stop routing before stopping the app)
#   2. mocker stop, wait for `mocker run` to exit, mocker rm
#   3. release the port lease
#   4. gate: re-check kernel state, then delete the app directory
#      with mockerhost-rmtree (never rm -rf)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MOCKER_BIN="$SCRIPT_DIR/../mocker"
RMTREE_BIN="$SCRIPT_DIR/mockerhost-rmtree"

MOCKERHOST_ROOT="/var/lib/mockerhost"
MOCKER_ROOT="/var/lib/mocker"
SUPERVISOR_WAIT_S=10

fail() {
    echo "TEARDOWN error: $1" >&2
    exit 1
}

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <app-name>" >&2
    exit 1
fi
NAME="$1"

# Same rule as mocker's registry and server.c: ^[a-zA-Z0-9_-]{1,64}$. Checked
# first, before $NAME is used to build any path. (mockerhost-rmtree and mocker
# re-validate on their own.)
case "$NAME" in
    "" | *[!a-zA-Z0-9_-]*) fail "invalid_name_allowed_chars_are_a-z_A-Z_0-9_underscore_dash" ;;
esac
if [ "${#NAME}" -gt 64 ]; then
    fail "invalid_name_longer_than_64_chars"
fi

if [ "$(id -u)" -ne 0 ]; then
    fail "must_run_as_root_same_as_mocker_run"
fi

# Every path below comes from these constants plus the validated $NAME.
APP_DIR="$MOCKERHOST_ROOT/apps/$NAME"
NGINX_CONF="$MOCKERHOST_ROOT/nginx/apps/$NAME.conf"
LEASES_FILE="$MOCKERHOST_ROOT/ports/leases"
STATE_DIR="$MOCKER_ROOT/containers/$NAME"
STATE_FILE="$STATE_DIR/state.json"
CGROUP_DIR="/sys/fs/cgroup/mocker/$NAME"

has_lease() {
    [ -f "$LEASES_FILE" ] && awk -v n="$NAME" '$2 == n { found = 1 } END { exit !found }' "$LEASES_FILE"
}

# mockerhost only tears down what mockerhost created: it must have an app
# directory, an nginx conf, or a port lease for this name. A container someone
# started by hand with `mocker run` has none of these and is left alone.
if [ ! -e "$APP_DIR" ] && [ ! -L "$APP_DIR" ] && [ ! -e "$NGINX_CONF" ] && ! has_lease; then
    echo "TEARDOWN: '$NAME' is not a mockerhost app (no app dir, nginx conf, or port lease)."
    echo "          If it is a plain mocker container, use 'mocker stop' / 'mocker rm'."
    exit 3
fi

warnings=0

# --- 1. stop routing first -----------------------------------------------------
# Remove the conf before touching the container, so visitors see a clean 404
# rather than a 502 from a dead upstream.
if [ -e "$NGINX_CONF" ] || [ -L "$NGINX_CONF" ]; then
    rm -f -- "$NGINX_CONF" || fail "could_not_remove_nginx_conf"
    echo "TEARDOWN: removed $NGINX_CONF"
    if ! nginx -s reload > /dev/null 2>&1; then
        echo "TEARDOWN warning: 'nginx -s reload' failed; the conf is gone from disk, so the" >&2
        echo "                  next successful reload will drop the route" >&2
        warnings=1
    fi
fi

# --- 2. stop and remove the container ---------------------------------------------
# `mocker run` blocks for the container's whole life, and writes state.json one
# last time after the container exits, which would resurrect the registry entry
# if `mocker rm` ran first. So after `mocker stop`, wait for that supervisor
# process (found by its cmdline, not trusted from any file) to be gone.
supervisor_running() {
    for cmdline in /proc/[0-9]*/cmdline; do
        args="$(tr '\0' ' ' < "$cmdline" 2>/dev/null)" || continue
        case "$args" in
            *"mocker run --name $NAME "*) return 0 ;;
        esac
    done
    return 1
}

CONTAINER_PID=""
if [ -f "$STATE_FILE" ]; then
    CONTAINER_PID="$(sed -n 's/.*"pid": *\([0-9][0-9]*\).*/\1/p' "$STATE_FILE")"

    "$MOCKER_BIN" stop "$NAME" || fail "mocker_stop_failed_nothing_deleted"

    waited=0
    while supervisor_running && [ "$waited" -lt "$SUPERVISOR_WAIT_S" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if supervisor_running; then
        fail "mocker_run_supervisor_still_alive_after_${SUPERVISOR_WAIT_S}s_nothing_deleted"
    fi

    "$MOCKER_BIN" rm "$NAME" || fail "mocker_rm_failed_nothing_deleted"
else
    echo "TEARDOWN: no mocker registry entry for '$NAME' (container never started or already removed)"
fi

# --- 3. release the port lease -----------------------------------------------------
# Same lock file clone_and_build.sh's lease_port() uses. Matches the name as
# field 2 exactly, not as a pattern over the whole line.
if has_lease; then
    (
        flock -x 9
        if awk -v n="$NAME" '$2 != n' "$LEASES_FILE" > "$LEASES_FILE.tmp"; then
            mv "$LEASES_FILE.tmp" "$LEASES_FILE"
        else
            rm -f "$LEASES_FILE.tmp"
            exit 1
        fi
    ) 9>"$LEASES_FILE.lock"
    if [ "$?" -eq 0 ]; then
        echo "TEARDOWN: released port lease"
    else
        echo "TEARDOWN warning: could not rewrite $LEASES_FILE (stale lease left behind)" >&2
        warnings=1
    fi
fi

# --- 4. gate, then delete the app directory ------------------------------------------
# Only delete the rootfs copy if the kernel and registry agree nothing is
# using it anymore - each re-derived now, not assumed from the steps above.
if [ -e "$APP_DIR" ] || [ -L "$APP_DIR" ]; then
    [ ! -e "$STATE_DIR" ] || fail "registry_entry_still_exists_refusing_to_delete_rootfs"
    [ ! -e "$CGROUP_DIR" ] || fail "cgroup_still_exists_refusing_to_delete_rootfs"
    if [ -n "$CONTAINER_PID" ] && [ -e "/proc/$CONTAINER_PID" ]; then
        fail "container_pid_${CONTAINER_PID}_still_alive_refusing_to_delete_rootfs"
    fi
    if supervisor_running; then
        fail "mocker_run_supervisor_still_alive_refusing_to_delete_rootfs"
    fi

    if [ ! -x "$RMTREE_BIN" ]; then
        fail "mockerhost-rmtree_not_built_run_make_mockerhost/mockerhost-rmtree"
    fi
    "$RMTREE_BIN" "$NAME" || fail "rmtree_failed_app_dir_partly_left_fix_cause_and_rerun"
fi

if [ "$warnings" -ne 0 ]; then
    echo "TEARDOWN: '$NAME' removed, with warnings above"
else
    echo "TEARDOWN: '$NAME' removed"
fi
