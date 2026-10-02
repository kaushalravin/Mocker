# mockerhost teardown design

Status: approved and implemented (`mockerhost/teardown.sh`, `mockerhost/src/rmtree.c`); written on
Windows, not yet verified on the Lubuntu VM.

Design-before-code, same discipline `mocker rm` got in [architecture.md](architecture.md) §5.
Teardown is the first thing in this project that **deletes a directory tree full of untrusted
content**: each deploy's rootfs copy contains a `git clone` of an arbitrary repo plus whatever its
build step produced (symlinks, `node_modules`, read-only dirs, files created by root inside the
container). That is exactly the "cleanup routine targets the wrong filesystem" case
[filesystem-safety.md](filesystem-safety.md) and CLAUDE.md §9 exist for, so the shape is decided
here and reviewed before any code is written.

## 1. What a deploy created (what teardown must undo)

From `mockerhost/clone_and_build.sh`, per app `<name>`:

| # | Created by | Where | Undone by |
|---|---|---|---|
| 1 | deploy script | `/var/lib/mockerhost/nginx/apps/<name>.conf` + the reloaded nginx config | unlink + `nginx -s reload` |
| 2 | `mocker run` | running container: process, cgroup, veth, DNAT/FORWARD rules, IP lease, `/var/lib/mocker/containers/<name>/` | `mocker stop` then `mocker rm` (both verified) |
| 3 | deploy script | line `<port> <name>` in `/var/lib/mockerhost/ports/leases` | filter the line out under `flock()` |
| 4 | deploy script | `/var/lib/mockerhost/apps/<name>/` = `rootfs/` (private `cp -a` copy + `rootfs/app` clone) and `build.log` | **new**: safe tree removal (§4) |

Never touched: the base rootfs template (`$MOCKERHOST_BASE_ROOTFS`, `/root/test-rootfs`) and
anything else outside `/var/lib/mockerhost/apps/<name>/`. `cp -a` makes new inodes, so the copy
shares no storage with the template and deleting the copy cannot affect it (verified in the test
plan, §7).

## 2. Decisions needing your review

1. **Operator-only CLI, not a web button (recommended).** `teardown.sh <name>`, run with `sudo`
   on the VM. The control server has no authentication (CLAUDE.md §5 rules auth out), so a
   `POST /teardown` would let anyone on the LAN delete anyone's app. Deploy is already open, but
   deploy cannot destroy someone else's work; teardown can. A web button can be added later
   behind whatever access model is chosen.
2. **Deletion lives in a small C helper, not shell.** `mockerhost/src/rmtree.c` →
   `mockerhost/mockerhost-rmtree <name>`. It takes only a validated name (no path argument), has
   the root path compiled in, and does the `openat`/`unlinkat` walk required by
   filesystem-safety.md ("no `rm -rf`, ever"). Everything else (stop, rm, nginx, leases) stays in
   shell, matching `clone_and_build.sh`.
3. **Remove the existing `rm -rf` in `clone_and_build.sh`** (the "redeploy over an existing
   `$APP_ROOTFS`" branch). It already contradicts CLAUDE.md §9 (it does have a `realpath` prefix
   case-guard, but it is still `rm -rf`). Replace with: refuse (`app_dir_exists_run_teardown_first`)
   and let teardown be the only deletion path. App names carry a `time % 100000` suffix, so a
   collision is rare, and refusing is the safe answer.

## 3. Flow: `teardown.sh <name>`

Order is the reverse of deploy, and chosen so a failure at any point leaves a state that is safe
and re-runnable (§6):

0. **Validate** `<name>` against `^[a-zA-Z0-9_-]{1,64}$` in shell (`case` pattern), fail closed. The
   C helper and `mocker` re-validate independently. Then confirm mockerhost owns
   `<name>`: an app dir, nginx conf, or lease line must exist. A registry entry alone does not count
   (that is a container someone ran by hand), so it gets "not a mockerhost app", exit 3.
1. **Stop routing first.** `unlink` `nginx/apps/<name>.conf` (fixed directory, validated name),
   then `nginx -s reload`. Doing this before stopping the container means visitors get 404, not a
   502 from a dead upstream. A reload failure is reported but not fatal: the conf is already gone
   from disk, so the next successful reload applies it.
2. **Stop the container.** If `/var/lib/mocker/containers/<name>/state.json` exists:
   - Record the container's PID from it.
   - `./mocker stop <name>` (SIGTERM, then SIGKILL after 5s, and the deployed `sh -c` PID 1
     ignores SIGTERM, so expect about 5s).
   - Wait (bounded, 10s) until no process in `/proc/*/cmdline` looks like
     `mocker run --name <name> ...` (the background `mocker run` supervisor, found by cmdline rather
     than trusted from a file). If it is still there after 10s, abort with nothing deleted. This matters: `mocker run` writes
     `state.json` one last time after its child exits, and `registry_write()` re-creates the
     entry's directory, so `rm` run before the supervisor exits could be undone by it.
3. **Remove mocker's state.** `./mocker rm <name>` (cgroup, veth, port rules, IP lease, registry).
   **If this fails, abort here** and do not proceed to step 5 — see the gate in §5.
4. **Release the port lease.** Under `flock` on `ports/leases.lock`, rewrite the file without the
   line whose second field is exactly `<name>` (`awk '$2 != name'`, not a regex over the whole
   line, as the deploy script's `release_port` does), via the same tmp+`mv` pattern.
5. **Delete the app directory**, only if the gate in §5 passes: `mockerhost-rmtree <name>`.
6. Print a one-line summary of what was removed / skipped / failed. Exit 0 only if everything that
   existed was removed.

`mocker` itself is not modified. It is driven through its CLI via `argv` (no shell string built from
the name), same as `server.c`/`clone_and_build.sh` do.

## 4. The tree removal: `mockerhost-rmtree <name>`

Everything in filesystem-safety.md applies unconditionally. Concretely:

**Path construction.** The only input is `<name>`. The root is the constant
`/var/lib/mockerhost`. Re-validate `<name>` with the same regex (a name that fails never reaches
path code). There is no path argument, no environment override, no `MOCKERHOST_ROOT` variable.

**Prefix verification (before any deletion):**
1. `realpath("/var/lib/mockerhost/apps")` must equal that exact string. This catches
   `/var/lib/mockerhost` or `apps/` having been replaced by a symlink elsewhere.
2. `open("/var/lib/mockerhost/apps", O_DIRECTORY|O_NOFOLLOW)` → `appsfd`; then
   `openat(appsfd, <name>, O_DIRECTORY|O_NOFOLLOW)` → `rootfd`. A symlink at `apps/<name>` makes
   `O_NOFOLLOW` fail → refuse. A regular file there → refuse.
3. Refuse if the resolved target is (or is a parent of) `/`, `/etc`, `/home`, `/usr`, `/var`,
   `/boot`, or equals `/var/lib/mockerhost` or `apps` itself. This is belt-and-braces on top of
   (1)-(2), as filesystem-safety.md requires.
4. Record `st_dev` of `rootfd`. It must equal `st_dev` of `appsfd` (the app dir is not a mount
   point).

**Mount check (the pivot_root-specific hazard).** Read `/proc/self/mountinfo`; if any mount point
equals the target path or lies beneath it, refuse and touch nothing. Under normal operation there
are none: `mocker` makes `/` `MS_REC|MS_PRIVATE` *before* its bind mount of the rootfs onto itself,
so that mount lives only in the container's own mount namespace and never appears in the host's.
The check exists for the case where that assumption is wrong, because deleting through a live
mount would delete whatever it exposes.

**The walk (`unlinkat`/`openat` only, bottom-up, one directory fd per level):**
- Read each directory via `fdopendir()`/`readdir()` on its fd, skipping `.` and `..`.
- `fstatat(dirfd, entry, AT_SYMLINK_NOFOLLOW)` every entry before acting.
- **Directory:** `openat(dirfd, entry, O_RDONLY|O_DIRECTORY|O_NOFOLLOW)`, then `fstat` the new fd and
  require same `st_dev` as `rootfd` and same `st_ino` as the `lstat`'d entry (guards against the
  entry being swapped between `lstat` and `open`), recurse, then
  `unlinkat(dirfd, entry, AT_REMOVEDIR)`.
- **Everything else (regular file, symlink, device, fifo, socket):** `unlinkat(dirfd, entry, 0)`. A
  symlink is removed as a link and **never opened or traversed**, even if it points at `/`,
  `/etc`, or a directory inside the rootfs.
- Different `st_dev` on any subdirectory → abort (a mount appeared that the mountinfo check
  missed; touch nothing further).
- Depth limit (256) → abort rather than recurse unboundedly.
- Any error (`EACCES`, `EBUSY`, immutable bit, unexpected type) → **abort the whole operation**,
  report the path and errno, leave whatever remains in place (partial cleanup beats guessing).
  Running as root, permission-denied on read-only directories (`0555`, common in package caches)
  is not an issue.
- Everything inside `apps/<name>/` (`rootfs/`, `build.log`, anything unexpected) is removed by the
  same walk, then `apps/<name>` itself is `rmdir`'d. Each directory is re-read until a pass finds it
  empty, because deleting while iterating can make `readdir()` skip entries.
- No `rm`, no `system()`, no `popen()`, no shell involved.

Roughly 120-150 lines of C, readable top to bottom, in the style of the rest of `cli/`.

## 5. The gate between "stop things" and "delete the directory"

`mockerhost-rmtree` is only invoked when **all** of these hold, each re-derived from the kernel or
the registry at that moment (CLAUDE.md §10), not from earlier assumptions:
- `/var/lib/mocker/containers/<name>/` no longer exists (so `mocker rm` succeeded, or there never
  was a container).
- The PID that was recorded for `<name>` has no `/proc/<pid>` entry, and no `mocker run --name <name>`
  supervisor process exists.
- `/sys/fs/cgroup/mocker/<name>` no longer exists (so no process is still charged to the container).
- (Inside the helper) the mountinfo check in §4 passes.

If any fails, teardown stops before deleting and says which. Nothing is "force"-able.

## 6. Failure and re-run behaviour

Each step is idempotent and skips when its target is already gone, so after any failure the
operator fixes the cause and re-runs `teardown.sh <name>`:

| Failure | State left | Safe? |
|---|---|---|
| nginx reload fails | conf already removed; routing stale until next reload | yes |
| `mocker stop` can't stop it | container running, rootfs untouched | yes, nothing deleted |
| `mocker rm` fails | cgroup/veth/registry may remain, rootfs untouched | yes, gate (§5) blocks deletion |
| lease rewrite fails | stale lease line (port unavailable for reuse), rest cleaned | yes, cosmetic |
| `rmtree` aborts mid-walk | partly-deleted app dir, container already gone | yes, a re-run continues |

## 7. Verification plan (on the Lubuntu VM, against kernel state)

1. Deploy a real app via the form (the same static-site run as stage 6). Record: container PID,
   `ls /var/lib/mockerhost/apps/`, the lease line, nginx conf, and a base-rootfs fingerprint
   (`find /root/test-rootfs | wc -l` plus `ls -R | sha256sum`).
2. `sudo ./mockerhost/teardown.sh <name>`. Then check: `/proc/<pid>` gone; `/sys/fs/cgroup/mocker/<name>`
   gone; host veth gone; iptables DNAT/FORWARD for its port gone; both lease files lack it;
   nginx conf gone and `curl http://<vm-ip>/apps/<name>/` returns **404** (not 502);
   `/var/lib/mockerhost/apps/<name>` gone; **base rootfs fingerprint unchanged**.
3. **Symlink trap (the important one).** Before teardown, HOST COMMAND: create
   `/tmp/mockerhost-decoy/sentinel`, then plant `ln -s /tmp/mockerhost-decoy
   /var/lib/mockerhost/apps/<name>/rootfs/app/evil` and a symlink to a decoy *file* too. After
   teardown the decoy directory and sentinel must still exist.
4. **Mount refusal.** HOST COMMAND: `mount --bind /tmp/mockerhost-decoy
   /var/lib/mockerhost/apps/<name>/rootfs/app/mnt`, run teardown, expect a refusal naming the
   mount and the decoy untouched; then `umount` it by hand and re-run teardown to completion.
5. **Name validation:** `../x`, `a/b`, an empty string and a 65-char name are rejected; a valid name
   that does not exist (including `-rf`, which the name rule allows) gets "not a mockerhost app",
   exit 3. In every case nothing is touched. Also: a container started by hand with
   `mocker run --name hand1` is refused the same way and left running.
6. **Idempotence:** run teardown twice; the second reports "not a mockerhost app" (exit 3) and
   changes nothing.
7. **Deep tree and read-only dirs:** a rootfs containing `chmod 555` directories and a deep
   (>50 level) tree deletes completely.
8. Concurrency: teardown of one app while a second app keeps serving; the second is unaffected
   (its lease, nginx conf, rootfs, cgroup intact).

## 8. Out of scope (stated, not hidden)

- No web UI / HTTP endpoint for teardown (decision 1).
- No automatic teardown (TTL, on-failure) and no `teardown --all`/prune. A bulk delete is a
  larger blast radius and gets its own design pass if wanted.
- Deleting `build.log` along with the app is deliberate (it lives under the app dir); if logs
  should outlive teardown, that is a separate decision.
- Still no user namespace: a container could have created files owned by arbitrary UIDs inside its
  rootfs. Teardown runs as root and removes them regardless of owner. That is the intended outcome
  here but a reminder that this tree is untrusted data.
