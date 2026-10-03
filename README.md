# Mocker

A from-scratch mini container runtime for Linux, built directly on namespaces and
cgroups v2 — no Docker, no `runc`, no OCI runtime underneath it. College Operating
Systems project; the point is demonstrating process-isolation primitives by hand.

- [`cli/`](cli/) — the `mocker` CLI/runtime itself. This is the graded coursework.
- [`mockerhost/`](mockerhost/) — product layer on top of `mocker`: GitHub repo URL in,
  live hosted URL out, running inside a `mocker` container behind nginx.
- [`demo/`](demo/) — programs and a script that prove isolation/limits hold at the
  kernel level (memory-leak, fork-bomb, two-containers-side-by-side).
- [`docs/`](docs/) — design notes, lifecycle docs, safety model.

See [CLAUDE.md](CLAUDE.md) for the full design, scope, and build-order plan.

## What it demonstrates

| OS concept | Where it lives | How it's used |
|---|---|---|
| PID, mount, UTS, network namespaces | `cli/src/namespace/` | one `clone(CLONE_NEWPID\|CLONE_NEWNS\|CLONE_NEWUTS\|CLONE_NEWNET)` per container |
| Root filesystem switch | `cli/src/mount/` | private mount propagation, `pivot_root()`, fresh `/proc` |
| cgroups v2 (memory, CPU, pids) | `cli/src/cgroups/` | limits applied and the PID attached right after `clone()` |
| Virtual networking | `cli/src/network/` | bridge `mocker0`, veth pairs, NAT, per-container IPs, `--port` DNAT |
| Container lifecycle | `cli/src/main.c`, `cli/src/registry/` | `run`, `stop`, `rm`, `ps`, `inspect` |
| Deployment on top | `mockerhost/` | clone a repo, build it, run it in a container, proxy it with nginx |

## Building and running (Linux only)

```
make
sudo ./mocker run --name web1 --mem-mb 64 --pids 32 /root/test-rootfs -- /bin/sh
sudo ./mocker ps
sudo ./mocker stop web1 && sudo ./mocker rm web1
```

Requires a Linux kernel with cgroups v2 mounted (`mount | grep cgroup2`), root, and
`iproute2`/`iptables`. Namespace and cgroup syscalls do not exist on Windows or macOS;
this project is developed on Windows and built/tested on a Lubuntu VM. Rootfs setup
(BusyBox) and the full flag list are in [cli/README.md](cli/README.md).

## Demos

Each demo is checked against kernel state (`/proc/<pid>/ns/*`, `/sys/fs/cgroup/mocker/`),
not just `mocker`'s own output. Steps are in [demo/README.md](demo/README.md).

1. `leak` in a 64 MB container: the kernel OOM-kills it inside its cgroup, the host is untouched.
2. `forkbomb` in a 32-pid container: `fork()` is refused at `pids.max`, the host is untouched.
3. `demo/multi-container.sh`: two containers with different PIDs, namespaces, hostnames,
   filesystems, IPs and limits; one is OOM-killed while the other keeps running.

## Status

| Piece | State |
|---|---|
| Architecture and filesystem-safety docs | done |
| cgroups, namespaces + `pivot_root`, combined `mocker run` | done, VM-verified |
| Networking (bridge, veth, NAT, `--port`) | done, VM-verified |
| `stop` / `rm` / `ps` / `inspect` | done, VM-verified against live containers |
| `leak` / `forkbomb` demos | done, VM-verified |
| `mockerhost` deploy (static site from a GitHub repo) | done, VM-verified |
| `demo/multi-container.sh` | written, **pending VM verification** |
| `mockerhost/teardown.sh` | written, **pending VM verification** |
| `mocker logs` / `exec` / `stats` | not built (deferred) |

## Known limitations

These are real gaps, not details. Mocker's isolation is weaker than a VM's and weaker than
a mature runtime's.

- **Shared kernel.** A container is a set of namespaces and cgroup limits around a normal
  process. A kernel bug or a bad syscall policy affects the host. This is not a VM boundary.
- **No user namespace.** Root inside a container can be equivalent to root on the host.
  Anything that escapes the container's mount/PID view has full host privileges.
- **No syscall filtering or capability dropping.** No seccomp, no capability bounding set,
  no AppArmor/SELinux profile.
- **`mockerhost` builds are not sandboxed beyond `mocker run`.** A repo's build and run
  commands execute as root in a container with the limits above and nothing more.
  `mockerhost` is local-network only, has no authentication, and offers no web terminal
  because of this.
- **Networking is minimal.** One bridge, IPv4 only, no network policy between containers
  (they can reach each other and the host's bridge address).
- **Rootfs is supplied, not managed.** `mocker` pivots into a directory you give it. It
  never copies, builds or deletes it, and there are no image layers (no OverlayFS).
- **Not built:** `mocker logs`, `exec`, `stats`, `start`/`restart`/`update`/`prune`.

Filesystem cleanup is deliberately conservative: nothing uses `rm -rf`, and `mocker rm`
never touches the rootfs. See [docs/filesystem-safety.md](docs/filesystem-safety.md).
