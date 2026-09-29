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
for the `clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS)` + `pivot_root()` sequence
and for `/sys/fs/cgroup/mocker/*`.

```
sudo ./mocker run --name <name> [--mem-mb <n>] [--pids <n>] [--cpu-percent <n>] <rootfs-path> -- <cmd> [args...]
sudo ./cgroup-test
```

`<rootfs-path>` must already exist and be populated (a minimal BusyBox/Alpine
rootfs works for testing) — `mocker` does not build or fetch one for you yet;
that's later `mockerhost`/rootfs-assembly work. Example, once you have one at
`/root/test-rootfs`:

```
sudo ./mocker run --name test1 --mem-mb 64 --pids 32 --cpu-percent 50 /root/test-rootfs -- /bin/sh
```

`--name` is required and must match `^[a-zA-Z0-9_-]{1,64}$` (see
[../docs/filesystem-safety.md](../docs/filesystem-safety.md)) — it's the one
identifier every path `mocker` touches is built from: the cgroup at
`/sys/fs/cgroup/mocker/<name>`, and the registry entry at
`/var/lib/mocker/containers/<name>/state.json`. The container's hostname is
also set to `<name>`. `--mem-mb`/`--pids`/`--cpu-percent` are optional; an
omitted limit is left at the kernel default (unlimited), not zero.

Verify isolation *and* limits from a second host shell while it's running:
`ps aux` inside vs. outside, `hostname`, `readlink /proc/<pid>/ns/{pid,mnt,uts}`
compared between the container's PID 1 and a host process, and
`cat /sys/fs/cgroup/mocker/<name>/{memory.max,pids.max,cpu.max,memory.current}`
(per CLAUDE.md section 10 — check kernel-visible state, not just the tool's own
output). `cat /var/lib/mocker/containers/<name>/state.json` should show
`"status": "running"` while it's up and `"exited"` once `mocker run` returns.

`mocker run` does not clean up after itself — the cgroup directory and the
registry entry are deliberately left in place once the container exits.
Cleanup is `mocker rm`'s job; it's designed in
[../docs/architecture.md](../docs/architecture.md) section 5 but not yet
implemented (CLAUDE.md section 9 forbids improvising that logic inline).

## Layout

```
cli/
├── src/
│   ├── namespace/    # namespace.c: single clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS); uts.c
│   ├── mount/        # mount_setup.c: the 9-step pivot_root() sequence
│   ├── process/      # launch/wait wrapper around namespace.c
│   ├── cgroups/       # cgroup creation, limits, attach, cleanup, usage read
│   │   ├── cgroups.c/.h   # reusable module (stage 2), now linked into mocker
│   │   └── cgroups_test.c # standalone OOM-kill proof, still built separately
│   ├── registry/      # registry.c/.h: per-container state.json (stage 4)
│   └── main.c        # the real `mocker run` CLI (stage 4 "Combine")
└── README.md
```

Modules not yet present (`net_setup.c`, `proc_launch.c`) will land as their
build stages (5+) are reached. `mocker stop`/`rm`/`ps`/`inspect`/`logs`/`exec`
are CLI surface not built yet — `run` is the only subcommand so far.
