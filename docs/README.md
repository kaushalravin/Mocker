# docs

Design notes, lifecycle docs, and the filesystem safety model for Mocker.

- [architecture.md](architecture.md) — Stage 1 deliverable: process/namespace/cgroup/rootfs
  lifecycles, the exact `pivot_root()` syscall sequence, on-disk state layout, and the `mocker rm`
  cleanup design (design only — not implemented until its own stage).
- [filesystem-safety.md](filesystem-safety.md) — the concrete, always-in-force safety model
  required by [../CLAUDE.md](../CLAUDE.md) §9: name validation, path prefix checks, the no-`rm -rf`
  rule, and the checklist any new filesystem-touching code must pass before merging.
