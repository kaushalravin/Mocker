# mocker CLI

The core runtime. Creates and manages namespaced, cgroup-limited processes
("containers") directly via Linux syscalls — see [../CLAUDE.md](../CLAUDE.md)
for the full design and build-order.

## Build

From the repo root:

```
make
```

## Layout

```
cli/
├── src/
│   ├── namespace/   # PID, UTS (and later mount, net) namespace setup
│   ├── process/      # fork/wait wrapper around namespace + container setup
│   └── main.c        # CLI entrypoint
└── README.md
```

Modules not yet present (`cgroups.c`, `mount_setup.c`, `net_setup.c`,
`proc_launch.c`, `registry.c`) will land as their build stages are reached.
