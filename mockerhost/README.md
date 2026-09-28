# mockerhost

Product layer on top of the `mocker` CLI: takes a GitHub repository URL, clones
and builds it, runs it inside a `mocker` container, and exposes it through a
reverse proxy so the user gets back a live hosted URL.

**Status: deferred.** Per [../CLAUDE.md](../CLAUDE.md) stage plan, this layer is
not started until the core `mocker` runtime (namespaces + cgroups + `mocker run`)
is solid — stages 1-4. Nothing here yet.

Planned contents: `clone_and_build.*`, `proxy_config/`, `server.*`.
