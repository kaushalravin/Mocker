# mocker CLI

The core runtime. Creates and manages namespaced, cgroup-limited processes
("containers") directly via Linux syscalls — see [../CLAUDE.md](../CLAUDE.md)
for the full design and build-order.

## Build

From the repo root:

```
make              # builds ./mocker: mocker run = namespaces + rootfs + cgroups + registry
make cgroup-test  # builds ./cgroup-test, the standalone stage-2 cgroups OOM-kill proof
```

Both require Linux with cgroups v2 — build and run these on the Lubuntu VM, not
on the Windows dev machine. Both also need root: `mocker` needs `CAP_SYS_ADMIN`
for the `clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET)` + `pivot_root()`
sequence, for `/sys/fs/cgroup/mocker/*`, and (stage 5) for the `ip`/`iptables`
commands it shells out to. Make sure `iproute2` and `iptables` are installed on
the VM (`sudo apt-get install -y iproute2 iptables`) — likely already present,
but worth confirming.

```
sudo ./mocker run --name <name> [--mem-mb <n>] [--pids <n>] [--cpu-percent <n>] [--port <hostport>:<containerport>] <rootfs-path> -- <cmd> [args...]
sudo ./cgroup-test
```

`<rootfs-path>` must already exist and be populated (a minimal BusyBox/Alpine
rootfs works for testing) — `mocker` does not build or fetch one for you yet;
that's later `mockerhost`/rootfs-assembly work. A static BusyBox rootfs is the
quickest thing that works, and needs an empty `proc/` directory too (`mocker`
mounts `/proc` itself during `pivot_root`, but expects the mountpoint to
already exist in the rootfs):

```
sudo apt-get update && sudo apt-get install -y busybox-static
sudo mkdir -p /root/test-rootfs/bin /root/test-rootfs/proc
sudo cp "$(which busybox)" /root/test-rootfs/bin/busybox
cd /root/test-rootfs/bin && sudo ./busybox --install . && cd -
```

Use plain `--install` (hardlinks), **not** `--install -s` (symlinks) — BusyBox's symlink
installer points each applet at the binary's absolute host path (resolved via
`/proc/self/exe` at install time, e.g. `/root/test-rootfs/bin/busybox`). After
`pivot_root`, that path no longer exists from inside the container (`/root/test-rootfs`
*is* the container's `/` now), so every applet becomes a dangling symlink and
`execvp()` fails with `ENOENT`. Hardlinks have no path to break.

Then, once you have a rootfs at `/root/test-rootfs`:

```
sudo ./mocker run --name test1 --mem-mb 64 --pids 32 --cpu-percent 50 /root/test-rootfs -- /bin/sh
```

`--name` is required and must match `^[a-zA-Z0-9_-]{1,64}$` (see
[../docs/filesystem-safety.md](../docs/filesystem-safety.md)) — it's the one
identifier every path `mocker` touches is built from: the cgroup at
`/sys/fs/cgroup/mocker/<name>`, the registry entry at
`/var/lib/mocker/containers/<name>/state.json`, and (stage 5) the container's
veth pair names and IP lease. The container's hostname is also set to
`<name>`. `--mem-mb`/`--pids`/`--cpu-percent` are optional; an omitted limit is
left at the kernel default (unlimited), not zero. `--port <hostport>:<containerport>`
is also optional — it maps a host TCP port to a port inside the container via
iptables DNAT; omit it if the container doesn't need to be reachable from
outside.

Verify isolation *and* limits from a second host shell while it's running:
`ps aux` inside vs. outside, `hostname`, `readlink /proc/<pid>/ns/{pid,mnt,uts,net}`
compared between the container's PID 1 and a host process, and
`cat /sys/fs/cgroup/mocker/<name>/{memory.max,pids.max,cpu.max,memory.current}`
(per CLAUDE.md section 10 — check kernel-visible state, not just the tool's own
output). `cat /var/lib/mocker/containers/<name>/state.json` should show
`"status": "running"` while it's up and `"exited"` once `mocker run` returns.

Verify networking (stage 5) the same way — from kernel-visible state, not just
`mocker`'s own output:
- On the host: `ip link show mocker0` (bridge up), `ip addr show mocker0`
  (has `10.200.0.1/24`), `ip link show <hostveth>` (attached, up), and
  `cat /var/lib/mocker/network/leases` (shows `<name> <ip>`).
- Inside the container shell: `ip addr show eth0` (has the leased IP),
  `ip route` (default route via `10.200.0.1`), `ping -c 3 10.200.0.1`
  (reaches the bridge/gateway).
- From the host, with `--port <hostport>:<containerport>` and something
  listening on `<containerport>` inside the container: `curl localhost:<hostport>`
  should reach it. `sudo iptables -t nat -L -n` should show the PREROUTING/OUTPUT
  DNAT rules and the POSTROUTING MASQUERADE rule.

`mocker run` does not clean up after itself — the cgroup directory and the
registry entry are deliberately left in place once the container exits.
Cleanup is `mocker rm`'s job; it's designed in
[../docs/architecture.md](../docs/architecture.md) section 5 but not yet
implemented (CLAUDE.md section 9 forbids improvising that logic inline).

## Layout

```
cli/
├── src/
│   ├── namespace/    # namespace.c: single clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWNET); uts.c
│   ├── mount/        # mount_setup.c: the 9-step pivot_root() sequence
│   ├── process/      # launch/wait wrapper around namespace.c
│   ├── cgroups/       # cgroup creation, limits, attach, cleanup, usage read
│   │   ├── cgroups.c/.h   # reusable module (stage 2), now linked into mocker
│   │   └── cgroups_test.c # standalone OOM-kill proof, still built separately
│   ├── registry/      # registry.c/.h: per-container state.json (stage 4)
│   ├── network/       # net_setup.c/.h: bridge/veth/IP-lease/port-map (stage 5)
│   └── main.c        # the real `mocker run` CLI (stage 4 "Combine", now stage 5-aware)
└── README.md
```

Modules not yet present (`proc_launch.c`) will land as their build stages are
reached. `mocker stop`/`rm`/`ps`/`inspect`/`logs`/`exec` are CLI surface not
built yet — `run` is the only subcommand so far.
