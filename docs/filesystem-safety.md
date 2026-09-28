# Filesystem safety model

This is the concrete, always-in-force safety model required by [../CLAUDE.md](../CLAUDE.md) §9.
Every piece of code that touches the filesystem — now or later — must be checked against this
before it's written, not after. See [architecture.md](architecture.md) for where these checks plug
into the actual lifecycles.

## Why this exists

Mocker uses mount namespaces and `pivot_root()`. A careless cleanup routine can target the wrong
filesystem if paths are misunderstood — inside a new mount namespace, `/` is the container's root,
*not* the host's, and after `pivot_root()` the old host root isn't even at `/` anymore. This is the
single highest-consequence mistake this codebase can make, so it gets its own explicit checklist
rather than being folded into general coding conventions.

## The one controlled root

Mocker only ever creates or deletes files under one constant, hardcoded base path:

```
MOCKER_ROOT = "/var/lib/mocker"
```

Nothing Mocker deletes is ever outside this tree except the per-container cgroup directory under
`/sys/fs/cgroup/mocker/<name>/`, which follows the identical name-validation and prefix-check
rules below. No other host path is ever a deletion target, full stop.

## Name validation (the single highest-leverage check)

Every path Mocker builds is `MOCKER_ROOT + "/containers/" + <name> + ...` or
`"/sys/fs/cgroup/mocker/" + <name>`. `<name>` comes from the user (`--name` on `mocker run`, or a
positional arg on `mocker stop`/`rm`/`exec`/etc.) — it is the one piece of untrusted input that
every dangerous path is built from.

**Validate it once, at the CLI boundary, before it touches any path-building code:**

```
^[a-zA-Z0-9_-]{1,64}$
```

Reject anything else outright — no `/`, no `..`, no leading `-` (avoids flag injection if a name
ever reaches a shelled-out command), no empty string, no unicode tricks. A name that fails this
check never reaches any function that builds a path — error out immediately, don't sanitize and
continue.

## Prefix verification before every destructive operation

Before any recursive delete or `rmdir()`, regardless of how trusted the caller believes the path
already is:

1. `realpath()` the target.
2. Confirm the resolved path starts with `MOCKER_ROOT + "/containers/"` **and** is strictly a
   subdirectory of it (reject if the resolved path *equals* `MOCKER_ROOT` or
   `MOCKER_ROOT + "/containers"` — a name that somehow resolves to the container root itself is a
   bug, not a container to delete).
3. Refuse — fail closed, abort the whole operation, touch nothing — if the resolved path is `/`,
   or is (or is a parent of) any of: `/etc`, `/home`, `/usr`, `/var`, `/boot`, or any other
   host-critical directory. This check exists *in addition to* the prefix check, not instead of
   it — belt and suspenders against a symlink or a `realpath()` surprise.

This applies even to code that "already knows" the path is safe (e.g. built it from a
just-validated name two lines up) — the check is unconditional, not conditional on how the path
was derived, because the cost of being wrong here is deleting host state.

## No `rm -rf`, ever

Never shell out to `rm -rf` (or `system()`/`popen()` any deletion command) for container cleanup,
under any circumstance, including "just for a quick test during development." Use the syscalls
directly instead:

- `unlinkat()` for files and symlinks.
- `rmdir()` for (empty) directories, walked bottom-up.
- `openat()` to get directory file descriptors while walking a tree, instead of building string
  paths at every level.
- `mount()` / `umount2()` for anything mount-table related — never `umount` as a shelled-out
  command either.

A recursive delete is a manual directory walk using the calls above, not a wrapper around a shell
command.

## Never follow symlinks during destructive cleanup

While walking a directory for deletion, `lstat()` every entry before acting on it. If an entry is
a symlink:

- Unlink the symlink itself (it's just a file entry).
- **Never** `chdir()`/open/traverse through it to whatever it points at. A symlink inside a
  container's rootfs could point anywhere, including back out to the host — treating it as a
  regular directory during a recursive delete is exactly how a contained cleanup routine deletes
  something outside the container.

## Fail closed

Any ambiguity — a path that doesn't resolve where expected, a name that doesn't match the
validation pattern, a `realpath()` call that fails, a directory that isn't where the registry says
it should be — is a hard stop, not a "best effort, skip and continue." Partial cleanup that leaves
some state behind is always preferable to cleanup that guesses wrong and touches the host. When
code hits this kind of ambiguity, it errors out and explains what looked wrong, rather than
guessing.

## Development-time discipline (this applies to Claude Code sessions too)

Before running any privileged command while building this out:

- State what it changes.
- State whether it affects the host or only an isolated namespace.
- Verify paths first, don't assume.
- Label experiments explicitly as `HOST COMMAND` vs `CONTAINER COMMAND`.
- Never assume a throwaway dev environment (the Lubuntu VM included) makes a destructive mistake
  "safe to try" — verify the target path regardless of how disposable the environment is.

## Checklist for any new filesystem-touching code

Before merging code that creates, moves, or deletes anything:

- [ ] Does every path it builds start from a validated `<name>` (§ Name validation)?
- [ ] Does every destructive call go through the `MOCKER_ROOT` prefix check (§ Prefix
      verification), unconditionally?
- [ ] Is there any `rm -rf`, `system()`, or `popen()` involved in deletion? (Must be none.)
- [ ] Does the directory walk `lstat()` before acting, and refuse to follow symlinks?
- [ ] Does every failure path abort cleanly instead of continuing with a guess?

If any answer is "no" or "not sure," the code isn't ready — per CLAUDE.md §9, stop and explain the
risk instead of guessing.
