#!/bin/sh
# mockerhost deploy glue: clone, build and run in a mocker container, register with nginx.
# Design: docs/mockerhost-design.md. Expanding $BUILD_CMD/$RUN_CMD is safe because they are
# double-quoted (one literal argv word on the host) and only become shell syntax inside the
# container (section 5).
#
# Usage: clone_and_build.sh <name> <github_url> <build_cmd> <run_cmd> <container_port>
# Prints exactly one stdout line, "RESULT ok <name> <hostport>" or "RESULT error <reason>";
# everything else goes to build.log because server.c parses that line.

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

# Never reuse or delete an existing app dir: only teardown.sh's mockerhost-rmtree deletes
# (CLAUDE.md section 9). Checked before leasing a port so a refused redeploy can't touch an
# existing lease. Clean leftovers with: sudo sh mockerhost/teardown.sh <name>
if [ -e "$APP_ROOTFS" ] || [ -L "$APP_ROOTFS" ]; then
    fail "app_dir_exists_run_teardown_sh_first"
fi

# --- lease a host port (flock()-guarded, like mocker's net_alloc_ip()) ---
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

# --- give this app its own rootfs ---
# mocker run pivot_root()s straight into the path it is given, so deploys can't share
# $BASE_ROOTFS (they'd write into one directory, and the repo would be invisible after the
# pivot). Each deploy gets a `cp -a` copy (no OverlayFS) with the repo cloned to <copy>/app.
: > "$LOG_FILE"

if ! cp -a "$BASE_ROOTFS" "$APP_ROOTFS" >> "$LOG_FILE" 2>&1; then
    release_port
    fail "rootfs_copy_failed_see_${LOG_FILE}"
fi

if ! git clone --depth 1 "$GITHUB_URL" "$APP_ROOTFS/app" >> "$LOG_FILE" 2>&1; then
    release_port
    fail "git_clone_failed_see_${LOG_FILE}"
fi

# --- launch in the background ---
# `mocker run` blocks for the container's lifetime, so background it and carry on.
echo "DEPLOY: container command = [cd /app && $BUILD_CMD && $RUN_CMD]" >> "$LOG_FILE"

(
    "$MOCKER_BIN" run --name "$NAME" --mem-mb 512 --pids 256 \
        --port "$HOST_PORT:$CONTAINER_PORT" \
        "$APP_ROOTFS" -- sh -c "cd /app && $BUILD_CMD && $RUN_CMD" \
        >> "$LOG_FILE" 2>&1
) < /dev/null > /dev/null 2>&1 &

# --- confirm it started ---
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

# --- register with nginx ---
# Proxy to the container's own IP, not 127.0.0.1:$HOST_PORT: a connection through the --port
# OUTPUT DNAT keeps its loopback source and hangs (504).
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
