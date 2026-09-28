# Architecture & lifecycles

Stage 1 deliverable (see [../CLAUDE.md](../CLAUDE.md) §6): the process/namespace/cgroup/rootfs
lifecycles, decided on paper *before* stage 2/3 implementation touches them. Nothing here is code
yet — this is the plan the code has to match. See [filesystem-safety.md](filesystem-safety.md) for
the dedicated safety model this design is required to uphold.

## 1. On-disk state layout

Everything Mocker owns on the host lives under one controlled root:

```
/var/lib/mocker/
└── containers/
    └── <name>/
        ├── rootfs/       # this container's root filesystem (pivot_root target)
        └── state.json    # registry entry: pid, cgroup path, rootfs path, status
```

`/sys/fs/cgroup/mocker/<name>/` is the matching cgroup directory (kernel-mandated location,
outside `/var/lib/mocker`, but keyed by the same `<name>`).

**`<name>` is the single identifier that drives every path Mocker touches** — the rootfs
directory, the cgroup directory, and the registry entry are all named after it. That makes name
validation the highest-leverage safety check in the whole system: reject anything that isn't
`^[a-zA-Z0-9_-]{1,64}$` at the CLI boundary, once, before any path is built from it. No path is
ever built from raw user input again after that point — see
[filesystem-safety.md](filesystem-safety.md).

Registry is per-container (`state.json` inside that container's own directory), not one shared
file — a corrupt/partial write for one container can't affect another, and it keeps
"does this container's directory look sane" a local question during cleanup. `mocker inspect` and
`mocker stats` still re-derive ground truth from `/proc/<pid>/*` and
`/sys/fs/cgroup/mocker/<name>/*` at read time — the registry is bookkeeping (name → pid, paths,
status), never the source of truth for "is this actually isolated / actually limited" (per
CLAUDE.md §10).

## 2. Process lifecycle

- `mocker run` itself is the supervisor. It stays alive and `waitpid()`s the container's PID-1
  process — this is the process `mocker stop` will eventually signal, and the thing that notices
  when a container exits.
- The container's PID 1 is created with a **single `clone()` call** carrying
  `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS` (network namespace joins this flag set at stage 5).
  All namespace creation happens at that one clone boundary, not layered on afterward.
- Inside the child (now PID 1 in its own PID namespace): set hostname, do the mount/pivot_root
  sequence (§4 below), then `execvp()` the command the user asked for. **Not** a hardcoded shell —
  today's prototype hardcodes `/bin/bash`; stage 3/4 replaces that with the actual `--cmd` the CLI
  was given.
- `mocker exec <name> -- <cmd>` is a different path: `setns()` into the *existing* container's
  namespaces (opened from `/proc/<pid>/ns/*`), then `fork()` + `exec()` the requested command
  inside them. No new container process is created — this attaches to the one already running.

### Why the current prototype's shape has to change

`cli/src/` today does `fork()` → child calls `unshare(CLONE_NEWUTS)` → child calls
`clone(CLONE_NEWPID)`. That's three separate steps building up isolation piecemeal. The real
design needs `pivot_root()` and the fresh `/proc` mount to happen *inside* the new PID **and**
mount namespace at the same time — which means namespace creation has to be one `clone()` call
with every relevant `CLONE_NEW*` flag set together, not namespaces added one at a time across
nested `fork`/`unshare`/`clone` calls. This is the concrete thing stage 3 changes.

## 3. Cgroup lifecycle

1. Before anything else, verify cgroups v2 is mounted (`mount | grep cgroup2`) and fail closed
   (refuse to run) if not — never assume, never hardcode a v1 path.
2. `mkdir("/sys/fs/cgroup/mocker/<name>")`.
3. Write limits: `memory.max`, `cpu.max`, `pids.max` (plain `write()` to regular files — no
   special syscall).
4. **Attach before the container does anything expensive.** The parent (supervisor) writes the
   child's host-side PID into `cgroup.procs` immediately after `clone()` returns, *before* the
   child gets far into its own setup. `cgroup.procs` is a real global path, unaffected by the
   child's own new namespaces, so the parent can do this from outside — no need to attach from
   inside the child. This avoids a window where the container runs unconstrained.
5. Cleanup: a cgroup directory can only be `rmdir()`'d once it's empty (no processes left in it).
   That means cgroup cleanup can only happen *after* the container's process has actually exited —
   part of what `mocker stop`/`rm` waits on. Plain `rmdir()`, never `rm -rf`.

## 4. Rootfs / mount lifecycle — the `pivot_root()` sequence

This is the highest-consequence part of the whole codebase (CLAUDE.md §9), so the exact syscall
order is spelled out here, not improvised at implementation time.

Precondition: `/var/lib/mocker/containers/<name>/rootfs/` already exists and is populated (a
shared minimal base rootfs, e.g. BusyBox/Alpine, with the app directory bind-mounted or copied in
— the assembly mechanics are stage-3 implementation detail; the *location* is decided now).

Inside the child, after `clone(CLONE_NEWNS | ...)`, in this exact order:

1. `mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)` — detach this namespace's mount tree from
   host propagation *first*, before touching anything else. Nothing the container does to its
   mounts from here on can leak to the host, and nothing the host does can leak in.
2. `mount(rootfs_path, rootfs_path, NULL, MS_BIND, NULL)` — bind-mount the rootfs onto itself so
   `pivot_root()` sees it as an actual mount point (a plain directory doesn't qualify).
3. `mkdir(rootfs_path + "/.old_root")` — a directory inside the new root to receive the old one.
4. `chdir(rootfs_path)`.
5. `syscall(SYS_pivot_root, ".", ".old_root")` — swaps the mount tree root; old host root is now
   mounted at `/.old_root` *inside the new root*, new root is `/`. (Called via raw `syscall()`
   since glibc doesn't reliably wrap `pivot_root`.)
6. `chdir("/")` — re-anchor into the new root now that we've pivoted.
7. `mount("proc", "/proc", "proc", 0, NULL)` — fresh `/proc` for the new PID namespace. Without
   this, `ps`/`top`/anything reading `/proc` inside the container still sees the host's process
   tree.
8. `umount2("/.old_root", MNT_DETACH)` — lazily detach the old root *from this mount namespace
   only*. This does not touch the host's real root; it only removes the old root from the
   container's now-private mount table.
9. `rmdir("/.old_root")`.

After step 5, `/` in this namespace **is** the container's rootfs — there is no path from inside
the container back to the host's real root. The sequence above never calls `rm -rf` anywhere;
every step is `mount()`/`umount2()`/`mkdir()`/`rmdir()`/`chdir()`/`pivot_root()`, per CLAUDE.md §9.

## 5. `mocker rm` design (designed now, implemented later)

CLAUDE.md §9 explicitly forbids implementing `rm`/`prune` until the cleanup lifecycle is designed
and reviewed. This is that design; stage 3+ implements it, not before.

Preconditions: only valid on a container that is stopped (process exited, cgroup has no attached
processes).

1. Look up `<name>` in the registry. If it doesn't exist, error — nothing to do.
2. Resolve the container's rootfs path with `realpath()` and verify it starts with the
   `/var/lib/mocker/containers/` prefix *and* is a subdirectory of it (not equal to the prefix
   itself). Refuse — fail closed — if the resolved path is `/`, or any of `/etc`, `/home`, `/usr`,
   `/var`, `/boot`, or doesn't match the expected prefix at all. See
   [filesystem-safety.md](filesystem-safety.md) for the exact check.
3. Recursively remove the rootfs directory by walking it with `openat()`/directory-fd calls and
   `unlinkat()`/`rmdir()` on each entry — never a shelled-out `rm -rf`. Every entry is `lstat()`'d
   before acting on it; a symlink is unlinked as itself and **never followed** into whatever it
   points at.
4. `rmdir()` the (now-empty) `/sys/fs/cgroup/mocker/<name>` directory.
5. Delete the registry entry (`state.json` and the now-empty `<name>/` directory itself).

If any step fails validation, the whole operation aborts before touching anything — no partial
cleanup, no "best effort" deletion.

## 6. What stage 3 actually changes in `cli/src/`

- Collapse the current `fork()` → `unshare()` → `clone()` chain into one `clone()` with the full
  `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS` flag set (§2).
- Add `mount_setup.c` implementing the exact 9-step sequence in §4.
- Add `cgroups.c` implementing §3.
- Replace the hardcoded `/bin/bash` exec target in `pid.c` with the CLI-supplied command
  (`proc_launch.c`'s job).
- Add `registry.c` writing/reading the per-container `state.json` described in §1.

`mocker rm`/`prune` themselves are **not** built at stage 3 — only the primitives they'll need
(§5 stays a design until its own stage).
