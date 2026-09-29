# mocker CLI

The core runtime. Creates and manages namespaced, cgroup-limited processes
("containers") directly via Linux syscalls — see [../CLAUDE.md](../CLAUDE.md)
for the full design and build-order.

## Build

From the repo root:

```
make              # builds ./mocker (stage-3 test harness: PID+mount+UTS namespaces)
make cgroup-test  # builds ./cgroup-test, the standalone stage-2 cgroups proof
```

Both require Linux with cgroups v2 — build and run these on the Lubuntu VM, not
on the Windows dev machine. Both also need root: `mocker` needs `CAP_SYS_ADMIN`
for the `clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS)` + `pivot_root()` sequence,
and `cgroup-test` needs it for `/sys/fs/cgroup/mocker/*`.

```
sudo ./mocker <rootfs-path> -- <cmd> [args...]
sudo ./cgroup-test
```

`<rootfs-path>` must already exist and be populated (a minimal BusyBox/Alpine
rootfs works for testing) — `mocker` does not build or fetch one for you yet;
that's later `mockerhost`/rootfs-assembly work. Example, once you have one at
`/root/test-rootfs`:

```
sudo ./mocker /root/test-rootfs -- /bin/sh
```

Verify isolation from a second host shell while it's running: `ps aux` inside
vs. outside, `hostname`, and `readlink /proc/<pid>/ns/{pid,mnt,uts}` compared
between the container's PID 1 and a host process (per CLAUDE.md section 10 —
check kernel-visible state, not just the tool's own output).

## Layout

```
cli/
├── src/
│   ├── namespace/   # namespace.c: single clone(CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS); uts.c
│   ├── mount/         # mount_setup.c: the 9-step pivot_root() sequence
│   ├── process/      # launch/wait wrapper around namespace.c
│   ├── cgroups/       # cgroup creation, limits, attach, cleanup, usage read
│   │   ├── cgroups.c/.h   # reusable module (stage 2)
│   │   └── cgroups_test.c # standalone OOM-kill proof, not linked into mocker
│   └── main.c        # stage-3 test harness (`mocker <rootfs> -- <cmd>`), not the final CLI
└── README.md
```

Modules not yet present (`net_setup.c`, `proc_launch.c`, `registry.c`) will land
as their build stages are reached. `cgroups.c` isn't linked into the `mocker`
binary yet — that happens at the "Combine" stage
(see [../docs/architecture.md](../docs/architecture.md)).
