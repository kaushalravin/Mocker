#ifndef REGISTRY_H
#define REGISTRY_H

#include <limits.h>
#include <sys/types.h>

/*
 * The one controlled root all of Mocker's on-host state lives under - see
 * docs/filesystem-safety.md "The one controlled root". Every path this
 * module builds starts here; nothing it writes ever lands outside this tree.
 */
#define MOCKER_ROOT "/var/lib/mocker"

/* ^[a-zA-Z0-9_-]{1,64}$ plus a NUL - the one size every buffer meant to hold
 * a container name is declared with. */
#define REGISTRY_NAME_MAX 65

/*
 * Registry entry for one container, mirrored to
 * MOCKER_ROOT "/containers/<name>/state.json" - see docs/architecture.md
 * section 1. This is bookkeeping only (name -> pid, paths, status); it is
 * never the source of truth for whether a container is actually isolated or
 * limited (per CLAUDE.md section 10) - that's always re-derived from
 * /proc/<pid>/[files] and /sys/fs/cgroup/mocker/<name>/[files] at read time.
 *
 * host_port/container_port default to -1 ("no mapping"), not 0, matching the
 * rest of the codebase's -1-means-unset convention (cgroup_limits_t). `rm`
 * needs these to know exactly which iptables rules to remove (see
 * docs/architecture.md section 5) - port_map() adds them, so rm must be able
 * to reconstruct the identical arguments to `-D` them.
 */
typedef struct {
    const char *name;
    pid_t pid;
    const char *rootfs_path;
    const char *cgroup_path;
    const char *ip; /* "a.b.c.d", stage 5 - see docs/architecture.md section 7 */
    const char *status; /* "running" or "exited" */
    int host_port;      /* -1 = no port mapping */
    int container_port; /* -1 = no port mapping */
} registry_entry_t;

/*
 * Read-back counterpart to registry_entry_t: owns its own storage (fixed
 * buffers, not borrowed const char*) since registry_read() has nothing to
 * borrow from - it's reconstructing the entry from a file the caller doesn't
 * otherwise hold pointers into.
 */
typedef struct {
    char name[REGISTRY_NAME_MAX];
    pid_t pid;
    char rootfs_path[PATH_MAX];
    char cgroup_path[PATH_MAX];
    char ip[32];
    char status[16];
    int host_port;
    int container_port;
} registry_record_t;

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

/*
 * Reads MOCKER_ROOT/containers/<name>/state.json back into *out. Returns -1
 * (name invalid, no such container, or unreadable/malformed state.json)
 * without touching *out's validity for the caller to rely on.
 */
int registry_read(const char *name, registry_record_t *out);

/*
 * Lists every container name currently recorded under
 * MOCKER_ROOT/containers/ into names[0..return value), each a
 * REGISTRY_NAME_MAX buffer. Returns the count (0 if none recorded yet, which
 * is not an error), or -1 on a real error. Entries that aren't valid names
 * (shouldn't exist, but a hand-edited tree could produce one) are silently
 * skipped rather than failing the whole listing.
 */
int registry_list(char names[][REGISTRY_NAME_MAX], int max_names);

/*
 * Deletes MOCKER_ROOT/containers/<name>/ (state.json then the now-empty
 * directory) - the registry half of `mocker rm`, see docs/architecture.md
 * section 5. Verifies the resolved path is actually under
 * MOCKER_ROOT/containers/ before removing anything, per
 * docs/filesystem-safety.md's prefix-verification rule - even though this is
 * only ever a two-entry removal, never a recursive walk.
 */
int registry_remove(const char *name);

#endif
