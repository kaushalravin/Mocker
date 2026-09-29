#ifndef REGISTRY_H
#define REGISTRY_H

#include <sys/types.h>

/*
 * The one controlled root all of Mocker's on-host state lives under - see
 * docs/filesystem-safety.md "The one controlled root". Every path this
 * module builds starts here; nothing it writes ever lands outside this tree.
 */
#define MOCKER_ROOT "/var/lib/mocker"

/*
 * Registry entry for one container, mirrored to
 * MOCKER_ROOT "/containers/<name>/state.json" - see docs/architecture.md
 * section 1. This is bookkeeping only (name -> pid, paths, status); it is
 * never the source of truth for whether a container is actually isolated or
 * limited (per CLAUDE.md section 10) - that's always re-derived from
 * /proc/<pid>/[files] and /sys/fs/cgroup/mocker/<name>/[files] at read time.
 */
typedef struct {
    const char *name;
    pid_t pid;
    const char *rootfs_path;
    const char *cgroup_path;
    const char *ip; /* "a.b.c.d", stage 5 - see docs/architecture.md section 7 */
    const char *status; /* "running" or "exited" */
} registry_entry_t;

/*
 * The single CLI-boundary name check (docs/filesystem-safety.md "Name
 * validation"): ^[a-zA-Z0-9_-]{1,64}$. Every path built from a container
 * name - here and in cgroups.c - is built only after a name has passed this,
 * so call it first, once, before touching any path.
 */
int registry_valid_name(const char *name);

/*
 * Creates MOCKER_ROOT/containers/<name>/ if needed and (over)writes its
 * state.json from *entry. Safe to call again for the same container to
 * update status (e.g. "running" -> "exited").
 */
int registry_write(const registry_entry_t *entry);

#endif
