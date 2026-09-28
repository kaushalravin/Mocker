# demo

Small, intentionally-misbehaving programs used to prove that `mocker`'s
isolation and resource limits actually hold at the kernel level — not just
according to `mocker`'s own output.

Planned contents (see [../CLAUDE.md](../CLAUDE.md) section 3 and the live demo
script in `mocker-project-context.md`):

- `leak.c` — deliberate memory leak, used to trigger a cgroup memory-limit
  OOM-kill while other containers keep running untouched.
- `forkbomb.c` — deliberate fork-bomb, used to prove `pids.max` caps it without
  taking down the host.
- `demo-app/` — minimal test app for namespace isolation demos (PID/mount/UTS).

Nothing here yet — these land alongside the cgroups and namespace stages so
each limit has something to demonstrate it.
