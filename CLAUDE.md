# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 1. Project overview

Mocker is a from-scratch mini container runtime written in C, built directly on Linux namespaces
and cgroups v2 — no Docker, no `runc`, no OCI runtime underneath it. The primary deliverable is the
`mocker` CLI/runtime itself. A secondary layer, `mockerhost`, will eventually take a GitHub repo,
build it, run it inside a `mocker` container, and expose it through a reverse proxy.

Current state: repo structure is in place (`cli/`, `mockerhost/`, `demo/`, `docs/`). Stage 1
(architecture/safety docs, `docs/architecture.md` + `docs/filesystem-safety.md`), stage 2
(cgroups — `cli/src/cgroups/`, kernel-verified OOM-kill test), and stage 3 (PID + mount + UTS
namespaces via a single `clone(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS)` in
`cli/src/namespace/namespace.c`, plus the 9-step `pivot_root()` sequence in
`cli/src/mount/mount_setup.c`) are done and verified on the Lubuntu VM. Stage 4 ("Combine") is
also done and verified on the Lubuntu VM: `cgroups.c` is linked into the main binary,
`cli/src/main.c` is a real `mocker run` CLI (`--name`/`--mem-mb`/`--pids`/`--cpu-percent`, plus
`<rootfs-path> -- <cmd> [args...]`) that creates the cgroup, applies limits, launches the
namespaced+pivot_root'd container, attaches its PID to the cgroup immediately after `clone()`
returns, and records a `state.json` registry entry via `cli/src/registry/registry.c`
(per `docs/architecture.md` section 1). The container's hostname is now set to `--name` instead
of a hardcoded string. Verified end-to-end on the VM with a BusyBox rootfs (applets installed as
**hardlinks**, not symlinks — `busybox --install -s` breaks after `pivot_root` since the symlink
target is an absolute host path; see `cli/README.md`): hostname, `/`, and `getpid()` all correctly
reflect the container's namespaces from inside a live `/bin/sh`, and `memory.max`/`pids.max` and
`state.json` match what was requested. Stage 5 (networking) is also done and verified on the
Lubuntu VM: `cli/src/network/net_setup.c` (bridge `mocker0` + NAT setup, `flock()`-guarded IP
leasing from `10.200.0.0/24`, veth pair creation/naming, container-side interface configuration,
`--port` DNAT/FORWARD rules — all via `fork()`+`execvp()` of `ip`/`iptables`, never
`system()`/`popen()`) is linked into the main binary; `clone()` in `cli/src/namespace/namespace.c`
now also carries `CLONE_NEWNET`, with a `pipe()`-based handshake so the container's PID 1 blocks
until the parent has moved its veth peer into its network namespace and wired the host side, then
configures `eth0` itself before `pivot_root()`; `main.c` gained a `--port <hostport>:<containerport>`
flag and now allocates/wires networking around the existing cgroup-attach step, killing the
container on a host-side wiring failure the same way a cgroup-attach failure is already handled.
`registry.c`'s `state.json` now also records the container's leased IP. Verified end-to-end on the
VM: `eth0` inside the container carries its leased `10.200.0.x/24` address with a default route via
the bridge, `ping` to the gateway succeeds, `/proc/<pid>/ns/net` resolves to a namespace distinct
from the host's, `mocker0`/host-side veth/iptables NAT+DNAT rules all show up in kernel-visible
state on the host, and a port mapped with `--port` was reachable end-to-end with `curl`. See
`docs/architecture.md` section 7 for the full design. `mocker stop`/`rm`/`ps`/`inspect` are now
implemented (`cli/src/main.c`, per `docs/architecture.md` sections 5 and 8) and are **verified on
the Lubuntu VM against live containers**, checked against kernel state (`/proc/<pid>/ns/*`,
`/sys/fs/cgroup/mocker/`, `ip`, `iptables`, the lease file), not just the registry. `stop` sends `SIGTERM` then escalates to `SIGKILL` after a 5s
timeout (PID-namespace-init signal immunity, section 8); `rm` refuses a still-running container and
only cleans up cgroup/veth/port-rules/IP-lease/registry state — never the rootfs, which Mocker never
copies or owns; `ps`/`inspect` always re-derive liveness from `/proc/<pid>`, never trusting the
stored `status` string alone (CLAUDE.md section 10). `registry.c`/`.h` gained `registry_read()`/
`registry_list()`/`registry_remove()` and `host_port`/`container_port` fields on the registry entry;
`net_setup.c`/`.h` gained `net_release_ip()`/`net_veth_delete()`/`net_port_unmap()`. `mocker
logs`/`exec` are not built yet. Stage 6 (`mockerhost`, per `docs/mockerhost-design.md`) is now
**done and verified on the Lubuntu VM** with a plain static HTML/JS site deployed from a GitHub repo
(BusyBox `httpd -f -p 8000 -h /app`) and reached from the LAN at `http://<vm-ip>/apps/<name>/`.
Lessons from that run, already fixed in the code: `/bin/sh` there is dash (no multi-digit fds, no
`disown`); the stock nginx `default` site must be removed; nginx proxies to the container's own
`10.200.0.x:<containerport>` because a host-originated connection to `127.0.0.1:<hostport>` (the
`--port` OUTPUT DNAT) keeps a loopback source and hangs; the success link must target port 80, not
the :8080 control server. Description of what it does — `mockerhost/src/server.c` is a raw-socket
HTTP server (serves `mockerhost/static/index.html`'s deploy form, handles `POST /deploy` by
validating fields and `fork()`+`execvp()`-ing `mockerhost/clone_and_build.sh` via `/bin/sh`, never a
host shell string built from user input); `clone_and_build.sh` leases a host port
(`flock()`-guarded, `/var/lib/mockerhost/ports/leases`), gives the deploy its own private rootfs
via `cp -a "$MOCKERHOST_BASE_ROOTFS" .../rootfs` (never the shared template path directly - `mocker
run` `pivot_root()`s straight into whatever it's given, so a shared path would both collide across
concurrent deploys and leave the cloned repo unreachable once `pivot_root()` detaches the old root),
`git clone --depth 1`s into `.../rootfs/app`, launches `./mocker run ... -- sh -c "cd /app &&
$build_cmd && $run_cmd"` in the background, polls for `state.json`, and writes/reloads an nginx
`location` block under `/var/lib/mockerhost/nginx/apps/<name>.conf` (one-time site template at
`mockerhost/nginx/mockerhost.conf`). Teardown is an operator-only command (`mockerhost/teardown.sh` + `mockerhost/src/rmtree.c`, per `docs/mockerhost-teardown-design.md`; **implemented, not yet VM-verified**), local-network-only
(path-based routing at `http://<vm-ip>/apps/<name>/`, no public DNS/TLS), no terminal/exec from the
web UI (deferred — no user namespace yet, see section 5). Build target added to the top-level
`Makefile` (`mockerhost/mockerhost-server`, independent of `cli/src/*.c`). See
`mockerhost/README.md` for setup/build/run and `docs/mockerhost-design.md` for the full design,
including the shell-injection-safety reasoning in its section 5. Update this file as real structure and
decisions land.

## 2. Academic objective

This is a college Operating Systems project. The point is to demonstrate direct understanding of
Linux process-isolation primitives by implementing them by hand: namespace creation (`clone`,
`unshare`, `setns`), controlled root-filesystem switching (`pivot_root`), mount propagation, and
resource limiting via cgroups v2 (memory, CPU, PIDs). Prefer explaining these mechanisms over
hiding them behind abstractions — the implementation *is* the demonstration.

## 3. Architecture

- **mocker** — the core C runtime/CLI. Creates and manages namespaced, cgroup-limited processes
  ("containers"). This is where the academic weight lives.
- **mockerhost** — product layer on top of `mocker`: a web form takes a GitHub repo URL + build/run
  commands, clones and builds it, invokes `mocker run`, and fronts it with nginx. Stage 6, now
  VM-verified with a static site — see section 1 and `docs/mockerhost-design.md`.
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
│   │   ├── namespace/        # namespace.c (single-clone dispatcher, now +CLONE_NEWNET), uts.c
│   │   ├── mount/            # mount_setup.c (pivot_root sequence)
│   │   ├── cgroups/          # cgroups.c/.h (linked into mocker) + standalone cgroups_test.c
│   │   ├── process/          # launch/wait wrapper around namespace.c
│   │   ├── registry/         # registry.c/.h (per-container state.json, now +ip field)
│   │   ├── network/          # net_setup.c/.h (bridge/veth/IP-lease/port-map, stage 5)
│   │   ├── main.c            # the real `mocker run` CLI (stage 4 "Combine", now stage 5-aware)
│   │   # not yet split out: proc_launch.c
│   └── README.md
├── mockerhost/
│   ├── src/server.c          # raw-socket HTTP server: serves the form, handles POST /deploy
│   ├── static/index.html     # the deploy form (github url, build cmd, run cmd, container port)
│   ├── clone_and_build.sh    # clone + mocker run + nginx registration, invoked by server.c
│   ├── teardown.sh           # operator-only: stop/rm app, drop nginx conf + lease, rmtree rootfs
│   ├── src/rmtree.c          # mockerhost-rmtree: safe unlinkat()-based deletion of apps/<name>/
│   ├── nginx/mockerhost.conf # one-time nginx site template
│   └── README.md
├── demo/
│   ├── leak.c                # not started
│   ├── forkbomb.c
│   └── demo-app/
├── docs/
│   └── mockerhost-design.md  # stage 6 design (scope, request flow, shell-injection safety)
├── Makefile                  # top-level, builds cli/src/* -> ./mocker, mockerhost/src/server.c -> mockerhost/mockerhost-server
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

### Next steps (in order, after stage 6)

1. ~~Verify `mocker stop`/`rm`/`ps`/`inspect` on live containers~~ — done, VM-verified.
2. ~~Demo programs~~ — `demo/leak.c` and `demo/forkbomb.c` done, VM-verified: `memory.events`
   `oom_kill`, `pids.events` `max`, host unaffected (`demo/README.md`).
3. **Stage 7, the multi-container demo** — two containers with independent PIDs, filesystem views,
   hostnames, IPs/ports, and independently enforced cgroup limits; mostly a scripted demonstration
   of what stages 3-6 already provide.
4. **`mockerhost` teardown** — designed (`docs/mockerhost-teardown-design.md`, approved) and
   implemented: `mockerhost/teardown.sh <name>` stops/removes the container, drops the nginx conf and
   port lease, then deletes the per-deploy rootfs copy via `mockerhost/src/rmtree.c`
   (`unlinkat()`-based, no symlink following, refuses mounts; never `rm -rf`).
   **Written on Windows, not yet verified on the Lubuntu VM** - verify against kernel state
   (section 10) before marking done.
5. **Final write-up** — update `README.md` and `docs/`, and the known-limitations section: no user
   namespace, isolation is namespaces + cgroups only (shared kernel, not a VM boundary).

Deferred (not in the list above): `mocker logs`/`exec`/`stats`.

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
