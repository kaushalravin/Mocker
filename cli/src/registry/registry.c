#define _GNU_SOURCE

#include "registry.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int registry_valid_name(const char *name)
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

static int mkdir_if_missing(const char *path)
{
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        perror(path);
        return -1;
    }
    return 0;
}

/* MOCKER_ROOT/containers/<name>/ is fixed-depth (three mkdir()s from
 * MOCKER_ROOT), so this is just those three calls in order - no generic
 * "mkdir -p" helper needed for one call site. */
static int ensure_container_dir(const char *name, char *dir_out, size_t dir_out_size)
{
    if (mkdir_if_missing(MOCKER_ROOT) == -1)
        return -1;

    char containers_dir[PATH_MAX];
    snprintf(containers_dir, sizeof(containers_dir), "%s/containers", MOCKER_ROOT);
    if (mkdir_if_missing(containers_dir) == -1)
        return -1;

    int n = snprintf(dir_out, dir_out_size, "%s/%s", containers_dir, name);
    if (n < 0 || (size_t)n >= dir_out_size) {
        fprintf(stderr, "registry: path too long for container '%s'\n", name);
        return -1;
    }

    return mkdir_if_missing(dir_out);
}

int registry_write(const registry_entry_t *entry)
{
    if (!registry_valid_name(entry->name)) {
        fprintf(stderr, "registry: invalid container name '%s'\n",
                entry->name ? entry->name : "(null)");
        return -1;
    }

    char container_dir[PATH_MAX];
    if (ensure_container_dir(entry->name, container_dir, sizeof(container_dir)) == -1)
        return -1;

    char state_path[PATH_MAX];
    int n = snprintf(state_path, sizeof(state_path), "%s/state.json", container_dir);
    if (n < 0 || (size_t)n >= sizeof(state_path)) {
        fprintf(stderr, "registry: path too long for container '%s'\n", entry->name);
        return -1;
    }

    FILE *f = fopen(state_path, "w");
    if (f == NULL) {
        perror(state_path);
        return -1;
    }

    fprintf(f,
            "{\n"
            "  \"name\": \"%s\",\n"
            "  \"pid\": %d,\n"
            "  \"rootfs\": \"%s\",\n"
            "  \"cgroup\": \"%s\",\n"
            "  \"status\": \"%s\"\n"
            "}\n",
            entry->name, (int)entry->pid, entry->rootfs_path, entry->cgroup_path,
            entry->status);

    fclose(f);
    return 0;
}
