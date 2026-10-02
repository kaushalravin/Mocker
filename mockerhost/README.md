# mockerhost

Product layer on top of the `mocker` CLI: takes a GitHub repository URL, a build
command, and a run command from a web form, clones and builds the repo inside a
`mocker` container, and exposes it through nginx so anyone on the same network
as the Lubuntu VM gets back a live URL. Full design and the safety reasoning
behind it: [../docs/mockerhost-design.md](../docs/mockerhost-design.md).

**Status: implemented, not yet verified on the Lubuntu VM.** Written on Windows
(see [../CLAUDE.md](../CLAUDE.md) section 12) — treat as untested until
confirmed there, same discipline the `mocker stop`/`rm`/`ps`/`inspect` commands
went through first.

**Scope, decided explicitly:** local network only (no public DNS/TLS), deploy
only (no teardown/undeploy button yet — clean up a deployed app manually with
`mocker stop`/`mocker rm`), no terminal/exec access from the web UI (there is
still no user namespace — root in a container is root on the host, so a shell
reachable by anyone on the network is a host-compromise path). See
[../docs/mockerhost-design.md](../docs/mockerhost-design.md) section 1.

## Layout

```
mockerhost/
├── src/server.c        # raw-socket HTTP server: serves the form, handles POST /deploy
├── static/index.html   # the deploy form (github url, build cmd, run cmd, container port)
├── clone_and_build.sh  # clone + mocker run + nginx registration, shelled out to by server.c
├── nginx/mockerhost.conf  # one-time nginx site template (see Setup below)
└── README.md
```

## Prerequisites

- `./mocker` already built at the repo root (`make` from the repo root) — `mockerhost`
  is purely a consumer of the `mocker` CLI, never linked against its internals.
- A base rootfs *template* with whatever language runtime your apps need already
  installed (node, python, etc.). `mockerhost` copies this whole directory (`cp -a`,
  not a mount) into a private rootfs per deploy — see
  [../docs/mockerhost-design.md](../docs/mockerhost-design.md) section 3 for why —
  so point `MOCKERHOST_BASE_ROOTFS` at it, or leave it unset to default to
  `/root/test-rootfs`. See [../cli/README.md](../cli/README.md) for how to build a
  minimal BusyBox one; BusyBox alone is enough for the smoke test below, but a real
  app (`npm install`, `pip install`, ...) needs the actual runtime present too.
  Leave enough disk headroom for one full copy of this template per deploy.
- `nginx` and `git` installed (`sudo apt-get install -y nginx git`).
- `flock` (part of `util-linux`, already on any standard Ubuntu install).

## One-time setup (per VM)

```bash
sudo rm -f /etc/nginx/sites-enabled/default   # removes only the symlink to Ubuntu's stock site
sudo ln -sf "$(pwd)/mockerhost/nginx/mockerhost.conf" /etc/nginx/sites-enabled/mockerhost.conf
sudo nginx -t && sudo systemctl reload nginx
```

The stock `default` site also claims port 80 as `default_server`, so left in place it
answers every request itself with its own 404 and never reaches mockerhost's `location`
blocks (and `nginx -t` fails with "duplicate default server" if both are enabled).

This installs the shared nginx site whose `include /var/lib/mockerhost/nginx/apps/*.conf;`
directive is how each deploy adds itself to the proxy — it is not touched again
after this.

## Build and run

```bash
make                     # builds ./mocker at the repo root, if not already built
cd mockerhost
gcc -Wall -Wextra -o mockerhost-server src/server.c
sudo MOCKERHOST_BASE_ROOTFS=/root/test-rootfs ./mockerhost-server
```

(or `make mockerhost/mockerhost-server` from the repo root, then run the binary
from inside `mockerhost/` — it reads `static/index.html` and invokes
`clone_and_build.sh` relative to its own working directory.)

`sudo` is required for the same reason `mocker run` itself needs it — `clone_and_build.sh`
calls `./mocker run`, which needs `CAP_SYS_ADMIN` for namespaces/cgroups/`pivot_root`.

Then, from any machine on the same network as the VM:

```
http://<vm-ip>:8080/
```

Fill in the GitHub URL, build command, run command, and the port your app
listens on inside the container. On success you get back a link at
`http://<vm-ip>/apps/<name>/` (port 80, through nginx — not port 8080, which is
only the control-plane form).

## Quick smoke test (no language runtime needed)

The plain BusyBox `test-rootfs` from [../cli/README.md](../cli/README.md) already
includes an `httpd` applet, which is enough to exercise the whole pipeline (port
leasing, per-deploy rootfs copy, clone, `mocker run`, nginx registration) without
installing a real toolchain first. In the form, use:

- **GitHub repo URL**: any small public repo, e.g. `https://github.com/octocat/Hello-World`
- **Build command**: `true`
- **Run command**: `busybox httpd -f -p 8000 -h .`
- **Container port**: `8000`

(`cd /app` already happens for you before these run, so `-h .` serves the cloned
repo's own files.) A working deploy means `http://<vm-ip>/apps/<name>/` serves
that repo's files — confirms the mechanism end-to-end before pointing
`MOCKERHOST_BASE_ROOTFS` at a rootfs with node/python/etc. for a real app.

## Security notes

- `server.c` never builds a host shell command from form input — it validates
  each field, then `fork()`+`execvp()`s `clone_and_build.sh` with each field as
  its own `argv` element (never `system()`/`popen()`, never string-concatenated
  into a shell command — the same rule `net_setup.c` follows for `ip`/`iptables`).
- `build_cmd`/`run_cmd` **do** end up as shell code eventually — that's the
  feature (arbitrary build/run steps) — but only once, and only inside the
  container `mocker run` creates, never on the host. See
  [../docs/mockerhost-design.md](../docs/mockerhost-design.md) section 5 for the
  exact quoting discipline this depends on.
- `github_url` is restricted to exactly the `https://github.com/` prefix, which
  rules out `file://`/`ext::`-style git transports that can themselves execute
  commands.
- Arbitrary repository build steps are not sandboxed beyond whatever isolation
  `mocker run` itself provides (per [../CLAUDE.md](../CLAUDE.md) section 8) — and
  there is still no user namespace, so that isolation is real but not a VM-grade
  boundary.

## Known limitations

- No teardown flow — ports, nginx config files, and per-deploy rootfs copies
  accumulate until removed by hand (`mocker stop`/`mocker rm`, then delete the
  app's directory under `/var/lib/mockerhost/apps/` and its nginx config under
  `/var/lib/mockerhost/nginx/apps/`, then `nginx -s reload`).
- Each deploy costs a full `cp -a` of the base rootfs template — fine for a demo,
  not something that scales to many concurrent deploys or a large toolchain image
  without the copy itself becoming slow / eating disk.
- A deployed app's `state.json` appearing confirms `mocker run` started the
  container, not that the app inside has finished building and is listening yet
  — a slow install step can mean the returned URL 502s for a while first. Check
  `/var/lib/mockerhost/apps/<name>/build.log` if that happens.
- `server.c`'s request loop is single-threaded and sequential — deploys are
  handled one HTTP request at a time (deployed apps themselves run concurrently
  once launched; only the control server's own accept loop is sequential).
