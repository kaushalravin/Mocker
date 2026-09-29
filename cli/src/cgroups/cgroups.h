#ifndef CGROUPS_H
#define CGROUPS_H

#include <sys/types.h>

/*
 * Limits requested for one container's cgroup. -1 means "leave this
 * controller unset" (kernel default = unlimited), not "0".
 */
typedef struct {
    long mem_max_bytes;   /* memory.max, in bytes */
    long pids_max;        /* pids.max, process/thread count */
    int  cpu_max_percent; /* cpu.max as a % of one core, 1-100 */
} cgroup_limits_t;

/* True if /sys/fs/cgroup is the unified cgroups v2 hierarchy. Never assume
 * this - always check, since v1's file layout is completely different. */
int cgroups_v2_available(void);

/* Creates /sys/fs/cgroup/mocker/<name>. Fails if it already exists. */
int cgroup_create(const char *name);

/* Writes memory.max / pids.max / cpu.max for fields >= 0 in *limits. */
int cgroup_apply_limits(const char *name, const cgroup_limits_t *limits);

/* Moves an already-running process into the cgroup (writes cgroup.procs). */
int cgroup_attach(const char *name, pid_t pid);

/* rmdir()s the cgroup directory. The kernel refuses (EBUSY) if any process
 * is still attached - that's the kernel doing the safety check for us. */
int cgroup_remove(const char *name);

/* Generic escape hatch: write `value` into <cgroup-dir>/<file>. Used by
 * cgroup_apply_limits() internally, and available directly for controller
 * files that don't have a dedicated wrapper yet. */
int cgroup_write_file(const char *name, const char *file, const char *value);

/* Current resident memory / pid count, read straight from the kernel -
 * for `mocker stats`/`inspect` to report ground truth, not our own guess. */
long cgroup_get_memory_current(const char *name);
long cgroup_get_pids_current(const char *name);

#endif
