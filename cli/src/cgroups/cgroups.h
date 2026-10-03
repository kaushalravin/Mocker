#ifndef CGROUPS_H
#define CGROUPS_H

#include <stddef.h>
#include <sys/types.h>

/* Limits for one container's cgroup. -1 leaves that controller unset (unlimited), not 0. */
typedef struct {
    long mem_max_bytes;   /* memory.max, in bytes */
    long pids_max;        /* pids.max, process/thread count */
    int  cpu_max_percent; /* cpu.max as a % of one core, 1-100 */
} cgroup_limits_t;

/* True if /sys/fs/cgroup is the unified cgroups v2 hierarchy; always check, never assume. */
int cgroups_v2_available(void);

/* Writes /sys/fs/cgroup/mocker/<name> into out; -1 if name fails validation. */
int cgroup_dir_path(const char *name, char *out, size_t out_size);

/* Creates the cgroup directory; fails if it already exists. */
int cgroup_create(const char *name);

/* Writes memory.max / pids.max / cpu.max for fields >= 0 in *limits. */
int cgroup_apply_limits(const char *name, const cgroup_limits_t *limits);

/* Moves a running process into the cgroup (writes cgroup.procs). */
int cgroup_attach(const char *name, pid_t pid);

/* rmdir()s the cgroup; the kernel refuses (EBUSY) while any process is attached. */
int cgroup_remove(const char *name);

/* Writes value into <cgroup-dir>/<file>. */
int cgroup_write_file(const char *name, const char *file, const char *value);

/* Current memory / pid count read straight from the kernel. */
long cgroup_get_memory_current(const char *name);
long cgroup_get_pids_current(const char *name);

#endif
