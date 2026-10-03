# docs

Design notes, lifecycle docs, and the filesystem safety model for Mocker.

- [architecture.md](architecture.md) — process/namespace/cgroup/rootfs lifecycles, the exact
  `pivot_root()` syscall sequence, on-disk state layout, the `mocker rm` cleanup design, the
  network lifecycle (section 7) and `stop`/`ps`/`inspect` (section 8). Written first as a plan;
  implemented in stages 2-5.
- [filesystem-safety.md](filesystem-safety.md) — the concrete, always-in-force safety model
  required by [../CLAUDE.md](../CLAUDE.md) §9: name validation, path prefix checks, the no-`rm -rf`
  rule, and the checklist any new filesystem-touching code must pass before merging.
- [mockerhost-design.md](mockerhost-design.md) — stage 6: scope, request flow, and the
  shell-injection safety reasoning for the GitHub-to-URL deploy layer.
- [mockerhost-teardown-design.md](mockerhost-teardown-design.md) — design for the operator-only
  teardown (`mockerhost/teardown.sh` + `rmtree.c`).

Demo steps and pass criteria are in [../demo/README.md](../demo/README.md). The project
overview, status table and known limitations are in [../README.md](../README.md).
