#define _GNU_SOURCE

#include "cgroups.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CGROUP_BASE "/sys/fs/cgroup/mocker"

/* <name> is untrusted user input; validate it before building any path (docs/filesystem-safety.md). */
static int is_valid_name(const char *name)
{
    if (name == NULL)
        return 0;

    size_t len = strlen(name);
    if (len == 0 || len > 64)
        return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '_' && c != '-')
            return 0;
    }

    return 1;
}

int cgroup_dir_path(const char *name, char *out, size_t out_size)
{
    if (!is_valid_name(name)) {
        fprintf(stderr, "cgroups: invalid container name '%s'\n", name ? name : "(null)");
        return -1;
    }

    int n = snprintf(out, out_size, "%s/%s", CGROUP_BASE, name);
    if (n < 0 || (size_t)n >= out_size) {
        fprintf(stderr, "cgroups: path too long for container '%s'\n", name);
        return -1;
    }

    return 0;
}

int cgroups_v2_available(void)
{
    /* cgroup.controllers exists only on the unified (v2) hierarchy. */
    return access("/sys/fs/cgroup/cgroup.controllers", F_OK) == 0;
}

static int write_to_path(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror(path);
        return -1;
    }

    ssize_t len = (ssize_t)strlen(value);
    if (write(fd, value, len) != len) {
        perror(path);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/* Controllers don't propagate to children on their own; enable them once on the shared
 * parent. Re-enabling is a no-op. */
static int enable_child_controllers(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", CGROUP_BASE);

    if (write_to_path(path, "+memory +pids +cpu") == -1) {
        fprintf(stderr,
                "cgroups: could not enable controllers on %s - check "
                "`cat /sys/fs/cgroup/cgroup.controllers` includes memory, pids, cpu\n",
                CGROUP_BASE);
        return -1;
    }

    return 0;
}

int cgroup_create(const char *name)
{
    char dir[PATH_MAX];

    if (!cgroups_v2_available()) {
        fprintf(stderr,
                "cgroups: /sys/fs/cgroup is not cgroups v2 (expected on modern Ubuntu; "
                "check `mount | grep cgroup2`)\n");
        return -1;
    }

    if (cgroup_dir_path(name, dir, sizeof(dir)) == -1)
        return -1;

    /* The shared parent may already exist from an earlier container. */
    if (mkdir(CGROUP_BASE, 0755) == -1 && errno != EEXIST) {
        perror("mkdir " CGROUP_BASE);
        return -1;
    }

    if (enable_child_controllers() == -1)
        return -1;

    if (mkdir(dir, 0755) == -1) {
        perror(dir);
        return -1;
    }

    return 0;
}

int cgroup_write_file(const char *name, const char *file, const char *value)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];

    if (cgroup_dir_path(name, dir, sizeof(dir)) == -1)
        return -1;

    int n = snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "cgroups: path too long (%s/%s)\n", dir, file);
        return -1;
    }

    return write_to_path(path, value);
}

static long read_cgroup_file_long(const char *name, const char *file)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];

    if (cgroup_dir_path(name, dir, sizeof(dir)) == -1)
        return -1;

    int n = snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return -1;
    }

    long value;
    int matched = fscanf(f, "%ld", &value);
    fclose(f);

    return matched == 1 ? value : -1;
}

int cgroup_apply_limits(const char *name, const cgroup_limits_t *limits)
{
    char buf[64];

    if (limits->mem_max_bytes >= 0) {
        snprintf(buf, sizeof(buf), "%ld", limits->mem_max_bytes);
        if (cgroup_write_file(name, "memory.max", buf) == -1)
            return -1;

        /* Best-effort: also cap swap so memory.max is a hard wall (may be unsupported). */
        cgroup_write_file(name, "memory.swap.max", "0");
    }

    if (limits->pids_max >= 0) {
        snprintf(buf, sizeof(buf), "%ld", limits->pids_max);
        if (cgroup_write_file(name, "pids.max", buf) == -1)
            return -1;
    }

    if (limits->cpu_max_percent >= 0) {
        /* cpu.max is "<quota> <period>" in us; 100000 is the kernel's default period. */
        long period_us = 100000;
        long quota_us = (period_us * limits->cpu_max_percent) / 100;
        snprintf(buf, sizeof(buf), "%ld %ld", quota_us, period_us);
        if (cgroup_write_file(name, "cpu.max", buf) == -1)
            return -1;
    }

    return 0;
}

int cgroup_attach(const char *name, pid_t pid)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%d", (int)pid);
    return cgroup_write_file(name, "cgroup.procs", buf);
}

int cgroup_remove(const char *name)
{
    char dir[PATH_MAX];

    if (cgroup_dir_path(name, dir, sizeof(dir)) == -1)
        return -1;

    /* The kernel refuses (EBUSY) while processes remain, so no forced delete is needed. */
    if (rmdir(dir) == -1) {
        perror(dir);
        return -1;
    }

    return 0;
}

long cgroup_get_memory_current(const char *name)
{
    return read_cgroup_file_long(name, "memory.current");
}

long cgroup_get_pids_current(const char *name)
{
    return read_cgroup_file_long(name, "pids.current");
}
