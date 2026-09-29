# mockerhost design (stage 6)

Design-before-code, same discipline `mocker rm` got in [architecture.md](architecture.md) §5 before
implementation — `mockerhost` introduces a new class of risk stage 1-5 didn't have (it executes
strings a web visitor typed into a form), so the shape of that boundary is decided here first.

## 1. Scope, decided explicitly

- **Local network only.** The result is a URL reachable by anyone who can reach the VM's own IP
  (same LAN, or the host machine via VMware). No public DNS, no TLS, no port-forwarding through a
  home router — that's hosting infrastructure outside this project, not something `mockerhost`'s
  code can provide.
- **No terminal/exec access from the web UI.** `mocker exec` (once built) gives shell access into a
  running container via the CLI, for the operator, on the VM itself. Exposing that over the web
  UI is deliberately out of scope: there is still no user namespace (`CLAUDE.md` §5/§8) — root in a
  container is root on the host — so a shell reachable by anyone on the network is a host-compromise
  path, not just a container one. Revisit only after user namespaces exist.
- **Deploy only, no teardown flow yet.** No "undeploy" button. Cleanup of a deployed app is a manual
  `mocker stop`/`mocker rm` from the CLI for now.

## 2. Components

```
mockerhost/
├── src/server.c        # raw-socket HTTP server: serves the form, handles POST /deploy
├── static/index.html   # the deploy form (github url, build cmd, run cmd, container port)
├── clone_and_build.sh  # clone + mocker run + nginx registration, shelled out to by server.c
├── nginx/mockerhost.conf  # one-time nginx site template (see §6)
└── README.md
```

`server.c` is C, matching the rest of the project and keeping the one piece that talks raw sockets
in the same language as everything else that talks raw syscalls. `clone_and_build.sh` is shell —
it is glue between existing tools (`git`, `./mocker`, `nginx`), not something that benefits from C's
ceremony. `server.c` never runs a host shell built from user input: it validates form fields, then
`fork()`+`execvp()`s `clone_and_build.sh` with each field as its own `argv` element — the same
"never `system()`/`popen()`, never string-concatenate untrusted input into a shell command" rule
`net_setup.c` already follows for `ip`/`iptables` (`docs/architecture.md` §7).

## 3. On-disk layout

A separate root from `mocker`'s own `/var/lib/mocker` — `mockerhost` is a consumer of `mocker`
through its CLI, not part of its trusted internals, and its state shouldn't live inside `mocker`'s
own controlled directory:

```
/var/lib/mockerhost/
├── apps/
│   └── <name>/
│       ├── rootfs/      # private copy of $MOCKERHOST_BASE_ROOTFS for this deploy (cp -a),
│       │   └── app/     #   with the git clone target at rootfs/app - this is what
│       │                #   mocker run pivot_root()s into, so /app is reachable
│       │                #   from inside the container
│       └── build.log    # rootfs copy + clone + mocker run's own stdout/stderr,
│                         #   kept out of the control channel
├── ports/
│   └── leases           # flat "<port> <name>" file, flock()-guarded, same pattern as
│                         # mocker's net_alloc_ip() (architecture.md §7)
└── nginx/apps/
    └── <name>.conf       # one `location` block per deployed app (§6)
```

**Why a private rootfs copy per deploy, not the shared base rootfs directly:** `mocker run`
`pivot_root()`s straight into whatever path it's given — it doesn't copy or mount anything into it.
If every deploy passed `$MOCKERHOST_BASE_ROOTFS` verbatim, (a) concurrent containers would all be
writing into the literal same host directory, and (b) the cloned repo — sitting under
`/var/lib/mockerhost/apps/<name>/`, unrelated to the base rootfs — would be completely unreachable
from inside the container once `pivot_root()` detaches the old root. Each deploy therefore gets its
own full copy of the base rootfs (plain `cp -a`, not OverlayFS — `CLAUDE.md` §5 rules that out
anyway), with the repo cloned to `<copy>/app`, so `cd /app` inside the container's own `sh -c`
lands exactly on the cloned source. Costs one full copy of the toolchain rootfs per deploy — fine
at this project's scale, called out under §7 limitations.

## 4. Request flow (`POST /deploy`)

1. `server.c` reads the form body (`application/x-www-form-urlencoded`), URL-decodes
   `github_url` / `build_cmd` / `run_cmd` / `container_port`.
2. **Validation, before anything is executed:**
   - `github_url` must start with exactly `https://github.com/` — rejects `file://`, `ext::`, and
     other git transport schemes that can themselves execute arbitrary commands, and guarantees the
     value can never start with `-` (which would otherwise risk being parsed as a `git clone` flag).
   - `container_port` must be all digits, 1-65535.
   - `build_cmd`/`run_cmd` are length-capped but otherwise not content-restricted — they are meant
     to run arbitrary commands, that's the feature. The safety boundary is *where* they execute, not
     filtering their content (see §5).
   - An app name is derived from the repo name (slugified to `mocker`'s own
     `^[a-zA-Z0-9_-]{1,64}$`, per `docs/filesystem-safety.md`), with a numeric suffix so redeploys of
     the same repo don't collide.
3. `server.c` forks, `execvp("./clone_and_build.sh", [name, github_url, build_cmd, run_cmd,
   container_port])`, and reads the child's stdout through a pipe until it closes.
4. `clone_and_build.sh`:
   - Leases a free host port from `/var/lib/mockerhost/ports/leases` (`[20000, 21000]`, `flock()`
     read-modify-write, same shape as `net_alloc_ip()`).
   - `cp -a "$MOCKERHOST_BASE_ROOTFS" .../rootfs` — a private rootfs copy for this deploy (see §3
     for why this has to be a copy, not the shared base path).
   - `git clone --depth 1 "$github_url" .../rootfs/app`, output redirected to `build.log` — **not**
     to the script's own stdout, which is reserved for one final structured line (see §5).
   - Launches `./mocker run --name "$name" --port "$hostport:$containerport" ... .../rootfs -- sh -c
     "cd /app && $build_cmd && $run_cmd"` **in the background** (`&`, `disown`) — `mocker run`
     blocks for the container's whole lifetime, and the deployed app is meant to keep running after
     the HTTP response goes back to the browser, not exit when the script does.
   - Polls for `/var/lib/mocker/containers/<name>/state.json` to appear (confirms `mocker run`
     actually got the container up), releasing the port lease and reporting failure if it times out.
   - Writes `/var/lib/mockerhost/nginx/apps/<name>.conf` (one `location` block, §6) and reloads
     nginx.
   - Prints exactly one final line, `RESULT ok <name> <hostport>` or `RESULT error <reason>`.
5. `server.c` reads that line back, and responds to the browser with either the live URL
   (`http://<vm-ip>/apps/<name>/`) or the error.

## 5. Where user input is allowed to become a shell command, and where it never is

This is the one design point everything else in this doc protects: **`build_cmd`/`run_cmd` become
shell code exactly once, and it happens inside the container, not on the host.**

`clone_and_build.sh` writes the container launch as one line, with both variables double-quoted:

```sh
./mocker run --name "$name" --port "$hostport:$containerport" ... "$app_rootfs" -- sh -c "cd /app && $build_cmd && $run_cmd" &
```

Because `"$build_cmd"` and `"$run_cmd"` are quoted, bash expands each into exactly one literal
`argv` word passed to `./mocker` — it does not re-parse their contents as additional shell syntax on
the host, regardless of what characters they contain. `mocker run` in turn `execvp()`s `sh -c
"<that string>"` as the container's own PID 1, after `pivot_root()` — so the *only* place
`build_cmd`/`run_cmd` are ever interpreted as shell syntax is inside the namespaced, cgroup-limited
container being deployed. `clone_and_build.sh` must never `eval` these variables or pass them through
a second host-side `sh -c` — doing so would move that interpretation point onto the host and defeat
the whole point of this section.

## 6. Reverse proxy

One shared nginx site, installed once (manual, one-time setup — not done per-deploy):

```
server {
    listen 80;
    include /var/lib/mockerhost/nginx/apps/*.conf;
}
```

symlinked into `/etc/nginx/sites-enabled/` (see `mockerhost/README.md`). Each deploy just drops one
new file into `/var/lib/mockerhost/nginx/apps/` and reloads — no text-surgery on an existing config
file, matching the same "never improvise cleanup/edits on files whose current contents you're not
re-deriving" caution `filesystem-safety.md` applies to `mocker` itself. Routing is by **path**
(`/apps/<name>/`), not subdomain — a subdomain (`<name>.mocker.local`) would need DNS or `/etc/hosts`
entries on every visitor's machine, which nobody but the operator has; a path on the VM's own IP
works for anyone on the network with no extra setup.

## 7. Known limitations (state these, don't hide them)

- No teardown flow yet — ports, nginx config files, and per-deploy rootfs copies accumulate until
  removed by hand.
- The base rootfs pointed to by `MOCKERHOST_BASE_ROOTFS` needs whatever language runtime a given
  app requires (node/python/etc.) already installed in it — `mockerhost` copies that template
  per deploy (§3), it doesn't assemble one from scratch. One template, one toolchain.
- Each deploy costs a full `cp -a` of the base rootfs — fine at demo scale, but not something that
  scales to many concurrent deploys or a large toolchain image without disk/time cost becoming
  noticeable.
- "The container's `state.json` appeared" is confirmation `mocker run` started the container, **not**
  confirmation the app inside finished building and is actually listening yet — a slow `npm install`
  can mean the returned URL 502s for a while before the app comes up.
- Single-threaded, sequential request handling in `server.c` — deploys are handled one HTTP request
  at a time (the deployed apps themselves run concurrently in the background; only the control
  server's own request loop is sequential). Fine at this scope, called out as a real limitation.
