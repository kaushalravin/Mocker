# Architecture & lifecycles

Stage 1 deliverable (see [../CLAUDE.md](../CLAUDE.md) §6): the process/namespace/cgroup/rootfs
lifecycles, decided on paper *before* stage 2/3 implementation touches them. Nothing here is code
yet — this is the plan the code has to match. See [filesystem-safety.md](filesystem-safety.md) for
the dedicated safety model this design is required to uphold.

## 1. On-disk state layout

Everything Mocker owns on the host lives under one controlled root:

```
/var/lib/mocker/
├── containers/
│   └── <name>/
│       ├── rootfs/       # this container's root filesystem (pivot_root target)
│       └── state.json    # registry entry: pid, cgroup path, rootfs path, ip, status
└── network/
    └── leases            # flat "<name> <ip>" lease file, flock()-guarded (stage 5, section 7)
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

## 5. `mocker rm` design

CLAUDE.md §9 explicitly forbids implementing `rm`/`prune` until the cleanup lifecycle is designed
and reviewed. This is that design.

**Correction to the original stage-1 version of this section:** it assumed Mocker copies the
rootfs into `/var/lib/mocker/containers/<name>/rootfs/` and that `rm` deletes that copy. That was
never implemented — `mocker run` (stage 4) takes `<rootfs-path>` as a CLI argument, `realpath()`s
it, and `pivot_root()`s straight into it; Mocker never copies or takes ownership of it. Per
[filesystem-safety.md](filesystem-safety.md)'s rule that nothing gets recursively deleted unless
it's verified Mocker-owned, **`mocker rm` never touches the rootfs path at all.** It only cleans up
state Mocker itself created: the cgroup, the network state added in stage 5, and the registry
entry. If Mocker ever starts owning a copy of the rootfs, this section gets revised again before
that deletion is implemented — not before.

Preconditions: only valid on a container that is not running. "Not running" is re-derived from the
kernel at `rm` time, not trusted from `state.json`'s `status` field (CLAUDE.md §10) — check whether
`/proc/<pid>` (the PID recorded in the registry) still exists. If it does, refuse and tell the user
to `mocker stop` first.

1. Look up `<name>` in the registry (`registry_read()`). If it doesn't exist, error — nothing to do.
2. Verify `/proc/<pid>` does not exist. If it does, abort — refuse to remove a running container.
3. `rmdir()` `/sys/fs/cgroup/mocker/<name>` (`cgroup_remove()`, already implemented — the kernel
   itself refuses with `EBUSY` if any process is still attached, which is a second, independent
   check on top of step 2).
4. Delete the host-side veth end: `ip link delete <host_veth>` (name re-derived deterministically
   from `<name>` via `net_veth_names()`, not stored — nothing to look up). The container-side peer
   is already gone; it lived inside the container's network namespace, which the kernel tears down
   automatically once the container's last process in it exits. Idempotent: if the interface
   doesn't exist, treat that as already-clean, not an error.
5. If the registry entry recorded a `--port` mapping (stage 5 added `host_port`/`container_port`
   fields to `registry_entry_t` for exactly this), remove the three iptables rules `net_port_map()`
   added, using `-D` with the identical arguments `-A` was called with. If no port mapping was
   recorded, skip this step.
6. Release the container's IP lease: delete its line from `MOCKER_ROOT "/network/leases"`
   (`net_release_ip()`, same `flock()`-guarded read-modify-write pattern as `net_alloc_ip()`).
7. Delete the registry entry: `unlink()` `state.json`, then `rmdir()` the now-empty
   `<name>/` directory under `MOCKER_ROOT "/containers/"`. This *is* a Mocker-owned path under
   `MOCKER_ROOT`, so the normal prefix-verification rule in filesystem-safety.md applies here (even
   though there's no recursive walk needed — it's always exactly one file then one empty dir).

If any step fails, the whole operation aborts and reports what it got through — no partial "best
effort" cleanup that silently continues past a failure it can't explain.

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

## 7. Network lifecycle (stage 5)

Every container gets its own network namespace (`CLONE_NEWNET`, joining the flag set in `clone()`
alongside PID/mount/UTS — §2), wired to the host through a veth pair and a shared Linux bridge, so
containers can reach each other and the outside world, and the host can reach into a container
through a mapped port.

### Implementation choice: shell out to `ip`/`iptables`, not raw netlink

`CLONE_NEWNET` itself — the actual namespace-creation syscall — is hand-rolled, same as
PID/mount/UTS. Configuring what's *inside* that namespace (create the veth pair, move one end into
it, assign IPs, add routes, set up NAT) is done by `fork()`+`execvp()`-ing the host's `ip` and
`iptables` binaries rather than hand-rolling `AF_NETLINK`/`NETLINK_ROUTE` messages
(`RTM_NEWLINK`/`RTM_NEWADDR`/`RTM_NEWROUTE`/etc.) directly. This was an explicit, discussed
trade-off (not a default): raw rtnetlink is real additional C for every operation below, and the
concept being demonstrated at the OS level is namespace isolation (`CLONE_NEWNET`), not netlink
socket programming — `ip`/`iptables` are still doing the exact same syscalls under the hood, just
via a subprocess instead of an in-process socket. Every `ip`/`iptables` invocation below is run
through one small helper (`net_run(argv[])` in `net_setup.c`) that `fork()`s, `execvp()`s, and
`waitpid()`s, checking the exit status — never `system()`/`popen()` (same shell-injection concern
`filesystem-safety.md` calls out for deletion commands applies here: arguments are built from
already-validated names/IPs, never interpolated into a shell string).

### Bridge

One shared bridge, `mocker0`, created lazily and idempotently the first time `mocker run` needs it
(`ip link show mocker0` first; only `ip link add ... type bridge` if absent — never assume no other
container has run yet):

```
ip link add mocker0 type bridge
ip addr add 10.200.0.1/24 dev mocker0
ip link set mocker0 up
sysctl -w net.ipv4.ip_forward=1          # HOST COMMAND - host-wide, needed once
iptables -t nat -A POSTROUTING -s 10.200.0.0/24 ! -o mocker0 -j MASQUERADE   # outbound NAT, added
                                                                              # only if an
                                                                              # equivalent -C check
                                                                              # doesn't already
                                                                              # find it
```

10.200.0.0/24 is chosen specifically to be unlikely to collide with a typical home/VM NAT range
(192.168.x.x, 172.x). `mocker0` at `10.200.0.1` is every container's default gateway.

### Per-container IP allocation

No full IPAM — a flat lease file at `MOCKER_ROOT "/network/leases"` (one `<name> <ip>` line per
container, append-only, `flock()`-guarded around the read-modify-write so two concurrent
`mocker run`s can't pick the same address). Allocation scans existing leases for used last-octets
and picks the lowest free one in `[2, 254]`. This is bookkeeping only, same caveat as the
`state.json` registry (§1) — it is not re-derived from kernel state, so a manually-deleted lease
file entry could in principle cause a reused IP. Documented limitation, not solved now (cleanup as
a whole is `mocker rm`'s job, §5, not built yet).

### Per-container veth pair

Host-side and peer interface names must fit `IFNAMSIZ` (16 bytes, 15 usable) even though container
names can be up to 64 characters, so names are derived, not used verbatim:
`v<first-6-chars-of-name><4-hex-digit-djb2-hash>` (host end) and the same with a `p` prefix (peer,
temporary — see below). This keeps names traceable to the container while bounding length; two
container names sharing both their first 6 characters *and* colliding on the hash could in theory
collide, a known, accepted limitation at this scope (avoid near-identical long names in testing).

Sequence, run from the parent (`mocker run`) right after `clone()` returns — symmetric with
"attach to the cgroup before the container does anything expensive" (§3 point 4), same reasoning
applies to networking: the container must not run for long with no network config half-applied:

```
ip link add <hostveth> type veth peer name <peerveth>
ip link set <peerveth> netns <pid>       # moves the peer end into the container's netns
ip link set <hostveth> master mocker0
ip link set <hostveth> up
```

### Synchronization: parent must finish before the child configures its side

The child creates its own network namespace at `clone()` time (empty: only `lo`, down). It cannot
rename/configure the peer interface until the parent has moved it in via the steps above, which
need the child's PID — so there's a real ordering dependency across two processes. This is handled
with a pipe created before `clone()`: the child blocks reading one byte right after printing that
PID 1 is alive, *before* touching hostname/`pivot_root`; the parent writes one byte only after the
veth-move-and-bridge-attach sequence above has fully succeeded. This is the one place in the
codebase where parent and child must be explicitly synchronized rather than just racing safely (the
cgroup attach doesn't need this — writing `cgroup.procs` needs no child cooperation).

Container-side config runs from inside the child, *before* `pivot_root` — deliberately, because the
child's mount namespace is still a private copy of the host's mount table at that point (§4: only
`pivot_root` itself switches the visible filesystem), so `execvp("ip", ...)` inside the child still
finds the host's real `/sbin/ip` even though the eventual container rootfs (a bare BusyBox image)
has no `iproute2` of its own. No `setns()` needed either way — the child is *already* running inside
its own net namespace since `clone()` created it there directly:

```
ip link set <peerveth> name eth0
ip addr add <container-ip>/24 dev eth0
ip link set eth0 up
ip link set lo up
ip route add default via 10.200.0.1
```

### Port mapping

Optional `--port <hostport>:<containerport>` on `mocker run`. Implemented as DNAT, added once the
container's IP is known — both chains are needed because `PREROUTING` only sees packets arriving
from outside the host, not ones a process on the host itself originates (e.g. `curl
localhost:<hostport>` from the VM's own shell), which needs `OUTPUT` too:

```
iptables -t nat -A PREROUTING -p tcp --dport <hostport> -j DNAT --to-destination <ip>:<containerport>
iptables -t nat -A OUTPUT     -p tcp --dport <hostport> -j DNAT --to-destination <ip>:<containerport>
iptables -A FORWARD -p tcp -d <ip> --dport <containerport> -j ACCEPT
```

### Cleanup

`mocker run` on its own still leaves everything in place after a container exits — the bridge,
per-container veth's host end, iptables NAT/DNAT rules, and the leases file entry. A container's own
veth *peer* end is reclaimed automatically by the kernel when its network namespace is destroyed (the
container process exits and nothing else holds that namespace open), which also detaches the peer
from `mocker0` on the host side — but the host-side veth stub, the iptables rules, and the lease all
persist until `mocker rm` is run against that container. See §8 for `mocker rm`'s full design (§5 has
the non-network half: cgroup and registry cleanup).

## 8. `mocker stop` / `mocker ps` / `mocker inspect` design

Designed together with §5 since `rm` depends on `stop` having run first, and `ps`/`inspect` share
the same "registry is bookkeeping, kernel is truth" posture as everything else (CLAUDE.md §10).

### `mocker stop <name>`

1. Look up `<name>` in the registry; error if missing.
2. Send `SIGTERM` to the registered PID.
3. Poll `/proc/<pid>` for up to a short fixed timeout (e.g. 5s); if the process is still alive
   after that, send `SIGKILL`.
4. Update `state.json`'s `status` to `"exited"`.

Step 3's escalation isn't paranoia — it's required. The registered PID is PID 1 *inside its own PID
namespace* (per stage 3's design), and Linux gives the init process of a namespace the same
signal-disposition immunity real init has: a signal whose default action is "terminate" (`SIGTERM`
included) is **not delivered** unless that process installed a handler for it. Most shells/programs
don't install a `SIGTERM` handler, so `SIGTERM` alone will often do nothing and `stop` would hang
forever without the `SIGKILL` fallback. (This is the same reason real Docker's `docker stop` has a
grace-period-then-`SIGKILL` design — it's not a Mocker-specific quirk, it's how PID-namespace init
processes behave.)

Note this means `mocker run`'s own foreground `waitpid()` is what actually reaps the process once
it exits — `run` stays attached and blocked in `wait_for_process()` for the container's whole
lifetime (this already works today; nothing about `run` needs to change for `stop` to work against
it from a second shell).

### `mocker rm <name>`'s network cleanup

The network half of §5's cleanup sequence (steps 4-6 there): delete the host-side veth
(`net_veth_delete()`, `ip link delete <host_veth>`, idempotent if already gone), remove the exact
`--port` iptables rules if one was recorded (`net_port_unmap()`, `-D` with the same arguments
`net_port_map()` used to `-A` them), and release the IP lease (`net_release_ip()`, same
`flock()`-guarded read-modify-write pattern as `net_alloc_ip()`). The bridge (`mocker0`) itself is
shared, host-wide state — never torn down by removing one container, the same way it's only ever
created once, lazily, by `net_bridge_ensure()`.

### `mocker ps`

Lists every container under `MOCKER_ROOT "/containers/"`. For each, `registry_read()`s its
`state.json`, then re-derives whether it's actually still running by checking `/proc/<pid>` right
now — the displayed status is never just the stored `status` string, since that can go stale (a
container that was `kill -9`'d outside Mocker entirely would still say `"running"` in `state.json`
forever otherwise).

### `mocker inspect <name>`

Prints the registry entry (`registry_read()`) plus, for a still-running container, live
kernel-derived state: `/proc/<pid>/ns/{pid,mnt,uts,net}` (via `readlink()`, to show the actual
namespace identifiers, not just "yes it has namespaces"), and current cgroup usage
(`cgroup_get_memory_current()` / `cgroup_get_pids_current()`, both already implemented). Same
principle as `ps`: what's re-derivable from the kernel is re-derived, not read back out of
`state.json`.
