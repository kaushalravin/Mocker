#!/bin/bash
# Stage 7: two mocker containers side by side, proven isolated by kernel state (/proc,
# /sys/fs/cgroup, nsenter), not mocker's own output. HOST script, run as root from the repo root:
#     sudo bash demo/multi-container.sh
# Deletes nothing: the two rootfs copies are created once and reused.

set -u

MOCKER=./mocker
BASE_ROOTFS=${BASE_ROOTFS:-/root/test-rootfs}
ROOTFS_A=/root/demo-rootfs-alpha
ROOTFS_B=/root/demo-rootfs-beta
CG=/sys/fs/cgroup/mocker

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)"; exit 1; }
[ -x "$MOCKER" ] || { echo "build first: make"; exit 1; }
[ -x "$BASE_ROOTFS/bin/leak" ] || {
    echo "$BASE_ROOTFS/bin/leak missing - see demo/README.md (make demos + cp)"; exit 1; }

say() { printf '\n=== %s\n' "$*"; }
state_field() { sed -n "s/.*\"$2\": *\"\{0,1\}\([^\",]*\)\"\{0,1\}.*/\1/p" "/var/lib/mocker/containers/$1/state.json"; }

# Each container needs its own rootfs: mocker run pivot_root()s into the path it is given.
for r in "$ROOTFS_A" "$ROOTFS_B"; do
    [ -d "$r" ] || cp -a "$BASE_ROOTFS" "$r"
done

cleanup() {
    "$MOCKER" stop alpha >/dev/null 2>&1
    "$MOCKER" stop beta >/dev/null 2>&1
    wait 2>/dev/null
    "$MOCKER" rm alpha >/dev/null 2>&1
    "$MOCKER" rm beta >/dev/null 2>&1
}
trap cleanup EXIT
cleanup # clear leftovers from an earlier run

# --- launch ---
# alpha (64 MB): prints what it sees, idles 20s for inspection, then leaks until OOM-killed.
# beta (128 MB): idles; must survive alpha's death.
say "HOST COMMAND: launching alpha (64 MB, 20 pids) and beta (128 MB, 50 pids)"
"$MOCKER" run --name alpha --mem-mb 64 --pids 20 "$ROOTFS_A" -- sh -c \
    'echo "alpha sees: hostname=$(hostname) my-pid=$$"; echo "alpha sees pids: $(echo /proc/[0-9]*)"; echo alpha > /secret; sleep 20; exec /bin/leak' \
    > /tmp/mocker-demo-alpha.log 2>&1 &
"$MOCKER" run --name beta --mem-mb 128 --pids 50 "$ROOTFS_B" -- sh -c \
    'echo "beta sees: hostname=$(hostname) my-pid=$$"; echo "beta sees pids: $(echo /proc/[0-9]*)"; sleep 600' \
    > /tmp/mocker-demo-beta.log 2>&1 &

for n in alpha beta; do
    for _ in $(seq 1 20); do
        [ "$(state_field $n status)" = running ] && break
        sleep 1
    done
    [ "$(state_field $n status)" = running ] || { echo "$n did not start"; exit 1; }
done
sleep 1
PA=$(state_field alpha pid); PB=$(state_field beta pid)
IPA=$(state_field alpha ip); IPB=$(state_field beta ip)

say "1. Different PIDs - what each container sees from the inside"
cat /tmp/mocker-demo-alpha.log /tmp/mocker-demo-beta.log | grep sees
echo "(both are PID 1 in their own PID namespace; on the host they are $PA and $PB)"

say "2. Different namespaces - /proc/<pid>/ns inodes (host vs alpha vs beta)"
printf '%-6s %-22s %-22s %-22s\n' ns host alpha beta
for ns in pid mnt uts net; do
    printf '%-6s %-22s %-22s %-22s\n' "$ns" "$(readlink /proc/self/ns/$ns)" \
        "$(readlink /proc/$PA/ns/$ns)" "$(readlink /proc/$PB/ns/$ns)"
done

say "3. Different hostnames - read from inside each UTS namespace via nsenter"
echo "host : $(hostname)"
echo "alpha: $(nsenter -t "$PA" -u hostname)"
echo "beta : $(nsenter -t "$PB" -u hostname)"

say "4. Different filesystems - /secret was written by alpha only"
ls -l "$ROOTFS_A/secret" "$ROOTFS_B/secret" 2>&1 | sed 's/^/   /'
echo "   (alpha's root is $(readlink /proc/$PA/root), beta's is $(readlink /proc/$PB/root))"

say "5. Different IPs - and they can reach each other over the mocker0 bridge"
echo "alpha=$IPA beta=$IPB"
nsenter -t "$PA" -n ping -c 1 -W 2 "$IPB" | sed -n '2p;4p' | sed 's/^/   alpha -> beta: /'

say "6. Independent cgroup limits (kernel files)"
printf '%-8s %-14s %-12s\n' name memory.max pids.max
for n in alpha beta; do
    printf '%-8s %-14s %-12s\n' $n "$(cat $CG/$n/memory.max)" "$(cat $CG/$n/pids.max)"
done

say "7. Limits enforced independently - alpha leaks, only alpha dies"
echo "waiting for alpha to exceed its 64 MB limit..."
for _ in $(seq 1 60); do
    [ ! -d /proc/$PA ] && break
    sleep 1
done
echo "alpha memory.events: $(grep oom_kill $CG/alpha/memory.events)"
echo "beta  memory.events: $(grep oom_kill $CG/beta/memory.events)"
[ -d /proc/$PA ] && echo "alpha: still alive (limit did NOT work)" || echo "alpha: dead  (killed by its cgroup)"
[ -d /proc/$PB ] && echo "beta : alive (unaffected)"           || echo "beta : dead  (BAD)"
dmesg | grep -i 'oom' | tail -n 2 | sed 's/^/   dmesg: /'

say "Done - stopping and removing both containers (rootfs copies are kept)"
