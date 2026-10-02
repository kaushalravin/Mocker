#!/bin/sh
#
# mockerhost's clone+build+run+register glue. See docs/mockerhost-design.md
# for the full design, and section 5 specifically for why it's safe to expand
# $BUILD_CMD/$RUN_CMD below despite being raw user input - the short version:
# double-quoting keeps each one a single literal argv word on the host; they
# only become shell syntax once, inside the container ./mocker run creates.
#
# Usage: clone_and_build.sh <name> <github_url> <build_cmd> <run_cmd> <container_port>
# Invoked by mockerhost/src/server.c via "/bin/sh clone_and_build.sh ...", not
# via its own execute bit - see server.c's run_clone_and_build().
#
# Prints exactly ONE control line to this process's own stdout when done:
#   RESULT ok <name> <hostport>
#   RESULT error <reason>
# Everything else this script runs (git, ./mocker, the app's own build/run
# output) is redirected to build.log, never mixed into that stdout - server.c
# reads this process's stdout expecting only that one line.

set -u

if [ "$#" -ne 5 ]; then
    echo "RESULT error usage_name_url_buildcmd_runcmd_containerport"
    exit 1
fi

NAME="$1"
GITHUB_URL="$2"
BUILD_CMD="$3"
RUN_CMD="$4"
CONTAINER_PORT="$5"

MOCKERHOST_ROOT="/var/lib/mockerhost"
MOCKER_BIN="../mocker"
BASE_ROOTFS="${MOCKERHOST_BASE_ROOTFS:-/root/test-rootfs}"
PORT_RANGE_START=20000
PORT_RANGE_END=21000
STATE_TIMEOUT_S=10

APP_DIR="$MOCKERHOST_ROOT/apps/$NAME"
APP_ROOTFS="$APP_DIR/rootfs"
LOG_FILE="$APP_DIR/build.log"
LEASES_FILE="$MOCKERHOST_ROOT/ports/leases"

fail() {
    echo "RESULT error $1"
    exit 1
}

mkdir -p "$MOCKERHOST_ROOT/ports" "$MOCKERHOST_ROOT/nginx/apps" "$APP_DIR"
touch "$LEASES_FILE"

if [ ! -x "$MOCKER_BIN" ]; then
    fail "mocker_binary_not_found_build_it_first_with_make"
fi

if [ ! -d "$BASE_ROOTFS" ]; then
    fail "base_rootfs_missing_set_MOCKERHOST_BASE_ROOTFS_or_prepare_root_test-rootfs"
fi

# --- lease a host port -------------------------------------------------------
# Same flock()-guarded scan-and-record shape as mocker's own net_alloc_ip()
# (cli/src/network/net_setup.c) - just in shell, since mockerhost's glue
# layer is shell by design (docs/mockerhost-design.md section 2).
lease_port() {
    (
        flock -x 9
        port="$PORT_RANGE_START"
        while [ "$port" -le "$PORT_RANGE_END" ]; do
            if ! grep -q "^$port " "$LEASES_FILE" 2>/dev/null; then
                echo "$port $NAME" >> "$LEASES_FILE"
                echo "$port"
                return 0
            fi
            port=$((port + 1))
        done
        return 1
    ) 9>"$LEASES_FILE.lock"
}

release_port() {
    (
        flock -x 9
        grep -v " $NAME\$" "$LEASES_FILE" > "$LEASES_FILE.tmp" 2>/dev/null || true
        mv "$LEASES_FILE.tmp" "$LEASES_FILE"
    ) 9>"$LEASES_FILE.lock"
}

HOST_PORT="$(lease_port)"
if [ -z "$HOST_PORT" ]; then
    fail "no_free_host_port_in_range"
fi

# --- give this app its own private rootfs -------------------------------------
# $BASE_ROOTFS is only a template - mocker run pivot_root()s directly into
# whatever rootfs path it's given, so if every deploy shared $BASE_ROOTFS
# verbatim, concurrent containers would be writing into the exact same host
# directory (and the cloned repo would never be reachable inside any of them,
# since it's cloned to $MOCKERHOST_ROOT which is unrelated to $BASE_ROOTFS -
# after pivot_root the container can't see it at all). Instead, each deploy
# gets its own full copy of the base rootfs (plain `cp -a`, not OverlayFS -
# CLAUDE.md section 5), with the repo cloned to <rootfs-copy>/app, so `cd
# /app` inside the container lands exactly on the cloned source.
if [ -e "$APP_ROOTFS" ]; then
    # Prefix-verify before removing anything, per docs/filesystem-safety.md's
    # rule applied to mockerhost's own state too - $APP_DIR is built entirely
    # from MOCKERHOST_ROOT and $NAME (already validated by server.c against
    # mocker's own ^[a-zA-Z0-9_-]{1,64}$ before this script is ever invoked),
    # never from unvalidated raw input.
    resolved_rootfs="$(realpath "$APP_ROOTFS" 2>/dev/null || echo "")"
    case "$resolved_rootfs" in
        "$MOCKERHOST_ROOT"/apps/*) rm -rf "$APP_ROOTFS" ;;
        *)
            release_port
            fail "refusing_to_reuse_unexpected_app_dir"
            ;;
    esac
fi

: > "$LOG_FILE"

if ! cp -a "$BASE_ROOTFS" "$APP_ROOTFS" >> "$LOG_FILE" 2>&1; then
    release_port
    fail "rootfs_copy_failed_see_${LOG_FILE}"
fi

if ! git clone --depth 1 "$GITHUB_URL" "$APP_ROOTFS/app" >> "$LOG_FILE" 2>&1; then
    release_port
    fail "git_clone_failed_see_${LOG_FILE}"
fi

# --- launch inside mocker, in the background ----------------------------------
# ./mocker run blocks for the container's whole lifetime (it's the foreground
# supervisor - docs/architecture.md section 2) - background it so this script
# can register the proxy and return, while the deployed app keeps running.
# Run from mockerhost/'s own cwd (not repo root) - $MOCKER_BIN is "../mocker"
# relative to here; $APP_ROOTFS is already an absolute path either way.
echo "DEPLOY: container command = [cd /app && $BUILD_CMD && $RUN_CMD]" >> "$LOG_FILE"

(
    "$MOCKER_BIN" run --name "$NAME" --mem-mb 512 --pids 256 \
        --port "$HOST_PORT:$CONTAINER_PORT" \
        "$APP_ROOTFS" -- sh -c "cd /app && $BUILD_CMD && $RUN_CMD" \
        >> "$LOG_FILE" 2>&1
) < /dev/null > /dev/null 2>&1 &

# --- confirm the container actually started ------------------------------------
STATE_FILE="/var/lib/mocker/containers/$NAME/state.json"
waited=0
while [ ! -f "$STATE_FILE" ] && [ "$waited" -lt "$STATE_TIMEOUT_S" ]; do
    sleep 1
    waited=$((waited + 1))
done

if [ ! -f "$STATE_FILE" ]; then
    release_port
    fail "container_did_not_start_within_${STATE_TIMEOUT_S}s_see_${LOG_FILE}"
fi

# --- register with the reverse proxy ---------------------------------------------
# Proxy straight to the container's own IP on the mocker0 bridge, NOT to
# 127.0.0.1:$HOST_PORT. mocker's --port rule is a DNAT in nat/OUTPUT, and a
# connection nginx opens to 127.0.0.1 keeps source 127.0.0.1 after the DNAT,
# which the kernel won't route out over mocker0 (and the container couldn't
# reply to) - it just hangs, so nginx reports a 504. The host can reach
# 10.200.0.x directly over the bridge with no NAT involved.
CONTAINER_IP="$(sed -n 's/.*"ip": *"\([0-9.]*\)".*/\1/p' "$STATE_FILE")"
if [ -z "$CONTAINER_IP" ]; then
    fail "could_not_read_container_ip_from_state_json"
fi

cat > "$MOCKERHOST_ROOT/nginx/apps/$NAME.conf" <<EOF
location /apps/$NAME/ {
    proxy_pass http://$CONTAINER_IP:$CONTAINER_PORT/;
}
EOF

if ! nginx -s reload > /dev/null 2>&1; then
    fail "nginx_reload_failed_app_is_running_but_not_yet_proxied"
fi

echo "RESULT ok $NAME $HOST_PORT"
