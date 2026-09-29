# mocker CLI

The core runtime. Creates and manages namespaced, cgroup-limited processes
("containers") directly via Linux syscalls — see [../CLAUDE.md](../CLAUDE.md)
for the full design and build-order.

## Build

From the repo root:

```
make              # builds ./mocker (namespace prototype - stage 3 rewrites this)
make cgroup-test  # builds ./cgroup-test, the standalone stage-2 cgroups proof
```

Both require Linux with cgroups v2 — build and run these on the Lubuntu VM, not
on the Windows dev machine. `cgroup-test` also needs root (creating
`/sys/fs/cgroup/mocker/*` requires it):

```
sudo ./cgroup-test
```

## Layout

```
cli/
├── src/
│   ├── namespace/   # PID, UTS (and later mount, net) namespace setup
│   ├── process/      # fork/wait wrapper around namespace + container setup
│   ├── cgroups/       # cgroup creation, limits, attach, cleanup, usage read
│   │   ├── cgroups.c/.h   # reusable module (stage 2)
│   │   └── cgroups_test.c # standalone OOM-kill proof, not linked into mocker
│   └── main.c        # CLI entrypoint (still the namespace prototype)
└── README.md
```

Modules not yet present (`mount_setup.c`, `net_setup.c`, `proc_launch.c`,
`registry.c`) will land as their build stages are reached. `cgroups.c` isn't
linked into the `mocker` binary yet — that happens at the "Combine" stage
(see [../docs/architecture.md](../docs/architecture.md)).
