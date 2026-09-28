# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 1. Project overview

Mocker is a from-scratch mini container runtime written in C, built directly on Linux namespaces
and cgroups v2 — no Docker, no `runc`, no OCI runtime underneath it. The primary deliverable is the
`mocker` CLI/runtime itself. A secondary layer, `mockerhost`, will eventually take a GitHub repo,
build it, run it inside a `mocker` container, and expose it through a reverse proxy.

Current state: repo structure is in place (`cli/`, `mockerhost/`, `demo/`, `docs/`) and `cli/src/`
has an early prototype — `fork()` + `unshare(CLONE_NEWUTS)`/`sethostname()` + `clone(CLONE_NEWPID)`
spawning a shell as PID 1. No mount namespace/`pivot_root`, no cgroups, and no arbitrary-command
exec yet. That's Stage 1+ work. Update this file as real structure and decisions land.

## 2. Academic objective

This is a college Operating Systems project. The point is to demonstrate direct understanding of
Linux process-isolation primitives by implementing them by hand: namespace creation (`clone`,
`unshare`, `setns`), controlled root-filesystem switching (`pivot_root`), mount propagation, and
resource limiting via cgroups v2 (memory, CPU, PIDs). Prefer explaining these mechanisms over
hiding them behind abstractions — the implementation *is* the demonstration.

## 3. Architecture

- **mocker** — the core C runtime/CLI. Creates and manages namespaced, cgroup-limited processes
  ("containers"). This is where the academic weight lives.
- **mockerhost** — later-stage product layer: clone a GitHub repo, build it, invoke `mocker run`,
  front it with a reverse proxy. Deferred until the core runtime works.
- **demo** — small intentionally-misbehaving programs (`leak.c`, `forkbomb.c`) and a demo app, used
  to prove isolation/limits actually hold (e.g. a forkbomb inside a container must not take down
  the host, `pids.max` must cap it).
- **docs** — design notes, lifecycle docs, safety model.

Backend services and databases are explicitly out of scope until the core runtime and frontend
deployment path work. Do not introduce them speculatively.

## 4. Repository structure

Live structure, top-level in this repo (no extra nesting under a `mocker/` folder — this repo
root *is* mocker). Adjust only if inspection of real content later shows a better fit — do not
restructure blindly once code exists:

```
Mocker/                       # repo root
├── cli/
│   ├── src/
│   │   ├── namespace/        # namespace.c (dispatcher), uts.c, pid.c (mount/net later)
│   │   ├── process/          # fork/wait wrapper around namespace setup
│   │   ├── main.c
│   │   # not yet split out: cgroups.c, mount_setup.c, net_setup.c, proc_launch.c, registry.c
│   └── README.md
├── mockerhost/
│   ├── clone_and_build.*     # not started
│   ├── proxy_config/
│   ├── server.*
│   └── README.md
├── demo/
│   ├── leak.c                # not started
│   ├── forkbomb.c
│   └── demo-app/
├── docs/
├── Makefile                  # top-level, builds cli/src/* -> ./mocker
├── CLAUDE.md
└── README.md
```

The Makefile stays at the repo root (not under `cli/`) since it's the single build entrypoint;
`cli/src/*.c` module names will grow to match the target list above as each build stage lands.

## 5. Technology constraints

- Language: C, for the `mocker` core.
- Target OS: Linux only, with **cgroups v2**. Never assume cgroups v1 — verify at runtime
  (`mount | grep cgroup2`, `cat /sys/fs/cgroup/cgroup.controllers`) rather than hardcoding paths.
- Namespaces in v1 scope: PID, mount, UTS, network, optionally IPC.
- **User namespace is a known v1 limitation — not implemented yet.** This means root inside a
  container can be equivalent to root on the host. Treat this as a real security boundary gap, not
  a detail to gloss over.
- Cgroup controllers in v1 scope: `memory`, `cpu`, `pids`.
- Explicitly do NOT introduce: xv6, Docker as an implementation dependency, `runc`, an OCI runtime,
  Kubernetes, microservices, unnecessary databases, authentication, unnecessary frameworks, or
  OverlayFS (unless explicitly approved). Avoid unnecessary abstractions generally — this is meant
  to stay readable and traceable to the underlying syscalls.

## 6. Development stages

Bottom-up, one subsystem tested independently before the next is built on top of it:

0. **Inspection** — survey whatever exists before touching it.
1. **Architecture & safety plan** — document process/namespace/cgroup/rootfs/`pivot_root`
   lifecycles and the filesystem safety model before writing implementation code.
2. **Cgroups** — creation, memory/CPU/PID limits, process attachment, usage inspection.
3. **PID + mount + UTS namespaces** — `clone()`, private mount propagation, controlled rootfs,
   fresh `/proc`, safe `pivot_root()`. Test each namespace independently.
4. **Combine** — `mocker run` = namespaces + rootfs + cgroups + exec, together.
5. **Networking** — veth pairs, bridge, container IPs, routing, port mapping. Highest-risk,
   most time-consuming subsystem; do not start before stage 4 is solid.
6. **Frontend deployment** — GitHub URL → clone → build → `mocker run` → reverse proxy → working
   URL.
7. **Multi-container demo** — two isolated containers with independent PIDs, filesystem views,
   hostnames, ports, and independently enforced cgroup limits.

CLI command MVP ordering (do not implement all commands up front):
1. `run`, `stop`, `rm`, `ps`, `inspect`, `logs`
2. `exec`, `stats`, `--pids-limit`
3. `start`, `restart`, `update`, `prune` (optional/later)

## 7. Coding conventions

Keep C code simple, readable, modular, and beginner-friendly — this is a learning project, not a
production runtime. Comment at the level of "why this syscall, why these flags," not line-by-line
narration. Prefer direct, syscall-oriented code over clever abstractions; three similar functions
across `namespaces.c`/`cgroups.c`/`mount_setup.c` beat one premature generic "subsystem manager."

## 8. Security rules (known limitations — state these, don't hide them)

- The container shares the host kernel — this is namespace/cgroup isolation, not a VM boundary.
- No user namespace in v1: root inside a container can equal root on the host.
- Arbitrary repository build steps (in `mockerhost`) are not sandboxed beyond whatever isolation
  `mocker run` itself provides.
- Do not describe Mocker's isolation as stronger than it actually is in docs or comments.

## 9. Filesystem safety rules (mandatory, always in force)

This project uses mount namespaces and `pivot_root()`. A careless cleanup routine after
`pivot_root()` can target the wrong filesystem if paths are misunderstood — this is the single
highest-consequence mistake this codebase can make.

- **Never** run or generate `rm -rf /`, `rm -rf /*`, or `rm -rf <unverified-path>`.
- **Never** assume `/` refers to the original host root after `pivot_root()` — inside the new mount
  namespace, `/` is the container's new root, and the old root has been moved/detached elsewhere.
- **Never** build container-rootfs cleanup around `rm -rf`. Use direct syscalls/APIs instead:
  `mount()`, `umount2()`, `unlinkat()`, `rmdir()`, `openat()`.
- **Never** recursively delete a path unless the code has explicitly verified it is a
  Mocker-owned directory (derived from a controlled Mocker state directory, not from raw user
  input).
- **Never** follow symlinks during destructive cleanup.
- **Refuse to operate** (fail closed) if a path unexpectedly resolves to `/`, `/etc`, `/home`,
  `/usr`, `/var`, `/boot`, or any other host-critical directory.
- Keep host-side rootfs storage clearly and structurally separated from the host's real `/`.
- Treat every path derived from user input (container names, app dirs, GitHub repo contents) as
  untrusted until validated.
- Do not implement `mocker rm` / `mocker prune` / any cleanup command until the filesystem
  lifecycle for that operation has been explicitly designed and reviewed — don't improvise cleanup
  logic inline.
- If there is any ambiguity about whether a filesystem operation could affect the host: **stop and
  explain the risk instead of guessing.**
- Before running any privileged command during development, explain what it changes, determine
  whether it affects the host or only an isolated namespace, and verify paths first. Label
  experiments `HOST COMMAND` vs `CONTAINER COMMAND` explicitly. Do not assume the dev environment
  (WSL2, a VM, a throwaway box) makes destructive mistakes safe.

## 10. Testing strategy

Every major subsystem is tested independently before integration, and tests must verify actual
kernel-visible state — not just trust Mocker's own registry. "Our program says it's isolated" and
"the kernel confirms it's isolated" are different claims; only the second counts as a passing test.

Inspect directly:
- `/proc/<pid>/ns/pid`, `/proc/<pid>/ns/mnt`, `/proc/<pid>/ns/uts`, `/proc/<pid>/ns/net`
- `/sys/fs/cgroup/mocker/<name>/` (limits and current usage)

## 11. Git workflow

Never run destructive git operations (`git reset --hard`, `git clean -fd`, `git checkout -- .`)
unless explicitly asked to in the moment. Do not discard existing work.

## 12. Development workflow (Windows edit / Linux VM test)

Claude Code runs on **Windows** in this repo. Code is written and reviewed here, but nothing in
`cli/` can actually build or run on Windows — namespace/cgroup syscalls are Linux-only. Actual
compilation and testing happens separately, by the user, on a **Lubuntu virtual machine in
VMware**, after cloning/pulling the commits made here.

- This is **not** a WSL2-based workflow. WSL2 is deliberately not used for this project (the user
  wants to avoid risking their existing WSL2 setup) — do not suggest testing via WSL2, and do not
  assume WSL2-specific paths, `wsl.conf` tweaks, or nested-NAT networking caveats apply.
- Do not run `make`, execute the built `mocker` binary, or run any privileged/namespace/cgroup
  command expecting it to work in this session — there is no Linux kernel here. Static review,
  syntax/logic checking, and `gcc -fsyntax-only`-style sanity checks (if a Linux toolchain happens
  to be available) are the practical limits of in-session verification.
- Treat every change as "written here, proven true later on the Lubuntu VM." Flag explicitly in
  responses when something is untested/unverified rather than implying it has been run.
- Because testing is on a separate machine, the plain root-branch `git clone`/`git pull` flow the
  user described is the update path — nothing here should assume a shared filesystem or live
  process between this Windows session and the VM.

## 13. Learning requirement

When implementing a major OS component, briefly explain: what it does, why it's needed, which
OS/Linux concept it demonstrates, and how it interacts with the other components already built.
Keep this tight — a few sentences, not a lecture.
