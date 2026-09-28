# Mocker

A from-scratch mini container runtime for Linux, built directly on namespaces and
cgroups v2 — no Docker, no `runc`, no OCI runtime underneath it. College Operating
Systems project; the point is demonstrating process-isolation primitives by hand.

- [`cli/`](cli/) — the `mocker` CLI/runtime itself. This is the graded coursework.
- [`mockerhost/`](mockerhost/) — later-stage product layer: GitHub repo URL in,
  live hosted URL out, running inside a `mocker` container. Deferred until the
  core runtime works.
- [`demo/`](demo/) — intentionally-misbehaving test payloads used to prove
  isolation/limits actually hold.
- [`docs/`](docs/) — design notes, lifecycle docs, safety model.

See [CLAUDE.md](CLAUDE.md) for the full design, scope, and build-order plan.

## Building (Linux only)

```
make
```

Produces a `mocker` binary. Requires a Linux kernel with cgroups v2 mounted
(`mount | grep cgroup2`) — namespace and cgroup syscalls used here do not exist
on Windows or macOS.
