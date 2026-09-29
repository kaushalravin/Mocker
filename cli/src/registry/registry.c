#define _GNU_SOURCE

#include "registry.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
            "  \"ip\": \"%s\",\n"
            "  \"status\": \"%s\",\n"
            "  \"host_port\": %d,\n"
            "  \"container_port\": %d\n"
            "}\n",
            entry->name, (int)entry->pid, entry->rootfs_path, entry->cgroup_path,
            entry->ip, entry->status, entry->host_port, entry->container_port);

    fclose(f);
    return 0;
}

/*
 * Line-oriented parser matched to registry_write()'s own fprintf() layout
 * above (one "key": value per line) - not a general JSON parser, since
 * state.json is only ever produced by registry_write() in the first place.
 */
int registry_read(const char *name, registry_record_t *out)
{
    if (!registry_valid_name(name)) {
        fprintf(stderr, "registry: invalid container name '%s'\n", name ? name : "(null)");
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->host_port = -1;
    out->container_port = -1;

    char state_path[PATH_MAX];
    int n = snprintf(state_path, sizeof(state_path), "%s/containers/%s/state.json", MOCKER_ROOT,
                      name);
    if (n < 0 || (size_t)n >= sizeof(state_path)) {
        fprintf(stderr, "registry: path too long for container '%s'\n", name);
        return -1;
    }

    FILE *f = fopen(state_path, "r");
    if (f == NULL)
        return -1; /* ENOENT here just means "no such container" - not worth perror() */

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        char key[64];
        char strval[400];
        int intval;

        if (sscanf(line, " \"%63[^\"]\" : \"%399[^\"]\"", key, strval) == 2) {
            /* Precision matched to each destination's own size (sizeof(dst) - 1),
             * not just "%s", so gcc can prove the copy fits and drop the
             * -Wformat-truncation= warning - snprintf() was already safe either way. */
            if (strcmp(key, "name") == 0)
                snprintf(out->name, sizeof(out->name), "%.64s", strval);
            else if (strcmp(key, "rootfs") == 0)
                snprintf(out->rootfs_path, sizeof(out->rootfs_path), "%s", strval);
            else if (strcmp(key, "cgroup") == 0)
                snprintf(out->cgroup_path, sizeof(out->cgroup_path), "%s", strval);
            else if (strcmp(key, "ip") == 0)
                snprintf(out->ip, sizeof(out->ip), "%.31s", strval);
            else if (strcmp(key, "status") == 0)
                snprintf(out->status, sizeof(out->status), "%.15s", strval);
        } else if (sscanf(line, " \"%63[^\"]\" : %d", key, &intval) == 2) {
            if (strcmp(key, "pid") == 0)
                out->pid = (pid_t)intval;
            else if (strcmp(key, "host_port") == 0)
                out->host_port = intval;
            else if (strcmp(key, "container_port") == 0)
                out->container_port = intval;
        }
    }

    fclose(f);

    if (out->name[0] == '\0') {
        fprintf(stderr, "registry: '%s' has no valid state.json\n", name);
        return -1;
    }

    return 0;
}

int registry_list(char names[][REGISTRY_NAME_MAX], int max_names)
{
    char containers_dir[PATH_MAX];
    snprintf(containers_dir, sizeof(containers_dir), "%s/containers", MOCKER_ROOT);

    DIR *d = opendir(containers_dir);
    if (d == NULL) {
        if (errno == ENOENT)
            return 0; /* nothing recorded yet - not an error */
        perror(containers_dir);
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while (count < max_names && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!registry_valid_name(ent->d_name))
            continue; /* not a name mocker could have created - skip rather than fail the listing */
        /* d_name could theoretically be up to NAME_MAX+1 bytes; registry_valid_name()
         * already rejected anything over 64 chars, but the precision here lets gcc
         * verify the copy fits REGISTRY_NAME_MAX without relying on that check alone. */
        snprintf(names[count], REGISTRY_NAME_MAX, "%.64s", ent->d_name);
        count++;
    }

    closedir(d);
    return count;
}

int registry_remove(const char *name)
{
    if (!registry_valid_name(name)) {
        fprintf(stderr, "registry: invalid container name '%s'\n", name ? name : "(null)");
        return -1;
    }

    char container_dir[PATH_MAX];
    snprintf(container_dir, sizeof(container_dir), "%s/containers/%s", MOCKER_ROOT, name);

    /* Prefix-verify before removing anything, per docs/filesystem-safety.md -
     * even though this is only ever unlink() + rmdir(), never a recursive
     * walk, the same "fail closed if it doesn't resolve where expected" rule
     * applies unconditionally. */
    char resolved[PATH_MAX];
    if (realpath(container_dir, resolved) == NULL) {
        perror(container_dir);
        return -1;
    }

    char expected_prefix[PATH_MAX];
    int n = snprintf(expected_prefix, sizeof(expected_prefix), "%s/containers/", MOCKER_ROOT);
    if (n < 0 || (size_t)n >= sizeof(expected_prefix))
        return -1;

    if (strncmp(resolved, expected_prefix, strlen(expected_prefix)) != 0) {
        fprintf(stderr,
                "registry: refusing to remove '%s' - resolved path '%s' is outside %s\n", name,
                resolved, expected_prefix);
        return -1;
    }

    /* Sized a few bytes past PATH_MAX (not just PATH_MAX) so gcc can prove
     * "<resolved>/state.json" can never truncate, even though resolved (from
     * realpath()) could itself be up to PATH_MAX-1 bytes long. */
    char state_path[PATH_MAX + 16];
    int state_n = snprintf(state_path, sizeof(state_path), "%s/state.json", resolved);
    if (state_n < 0 || (size_t)state_n >= sizeof(state_path)) {
        fprintf(stderr, "registry: path too long for container '%s'\n", name);
        return -1;
    }
    if (unlink(state_path) == -1 && errno != ENOENT) {
        perror(state_path);
        return -1;
    }

    if (rmdir(resolved) == -1 && errno != ENOENT) {
        perror(resolved);
        return -1;
    }

    return 0;
}
