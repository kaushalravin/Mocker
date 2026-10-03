#ifndef REGISTRY_H
#define REGISTRY_H

#include <limits.h>
#include <sys/types.h>

/* All of Mocker's on-host state lives under this root (docs/filesystem-safety.md). */
#define MOCKER_ROOT "/var/lib/mocker"

/* ^[a-zA-Z0-9_-]{1,64}$ plus a NUL. */
#define REGISTRY_NAME_MAX 65

/* One container's state.json. Bookkeeping only: isolation and limits are always
 * re-derived from /proc and /sys/fs/cgroup, never trusted from here. */
typedef struct {
    const char *name;
    pid_t pid;
    const char *rootfs_path;
    const char *cgroup_path;
    const char *ip; /* "a.b.c.d" */
    const char *status; /* "running" or "exited" */
    int host_port;      /* -1 = no port mapping */
    int container_port; /* -1 = no port mapping */
} registry_entry_t;

/* Read-back form of registry_entry_t, with its own storage. */
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

/* The CLI-boundary name check; call it before building any path from a name. */
int registry_valid_name(const char *name);

/* Creates containers/<name>/ if needed and (over)writes its state.json. */
int registry_write(const registry_entry_t *entry);

/* Reads state.json back into *out; -1 if the name is invalid or the file is missing/malformed. */
int registry_read(const char *name, registry_record_t *out);

/* Fills names[] with every recorded container; returns the count (0 is not an error) or -1. */
int registry_list(char names[][REGISTRY_NAME_MAX], int max_names);

/* Deletes state.json and the container's directory, after verifying the path is under
 * containers/. Never recursive. */
int registry_remove(const char *name);

#endif
