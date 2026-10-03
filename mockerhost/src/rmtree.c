#define _GNU_SOURCE

/* mockerhost-rmtree <name>: deletes /var/lib/mockerhost/apps/<name>/ and nothing else.
 * The one recursive delete over untrusted content, so it follows docs/filesystem-safety.md
 * (reasoning: docs/mockerhost-teardown-design.md section 4):
 *   - The only input is an app name; the root is a compile-time constant.
 *   - Everything is openat()/fstatat()/unlinkat() on directory fds with O_NOFOLLOW, so
 *     symlinks are unlinked, never followed.
 *   - It refuses mounts under the target and aborts if it crosses devices (st_dev).
 *   - Any error aborts, leaving a partial cleanup that can be re-run.
 * No rm, system(), popen() or shell. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MOCKERHOST_ROOT "/var/lib/mockerhost"
#define APPS_DIR        MOCKERHOST_ROOT "/apps"
#define NAME_MAX_LEN    64
#define MAX_DEPTH       256

static long files_removed;
static long dirs_removed;

/* Same rule as mocker's own registry_valid_name(): ^[a-zA-Z0-9_-]{1,64}$ */
static int valid_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX_LEN)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        if (!ok)
            return 0;
    }
    return 1;
}

/* Defense in depth: the target must never be, or contain, a host-critical directory. */
static int path_is_forbidden(const char *path)
{
    static const char *const forbidden[] = {"/",    "/etc", "/home",         "/usr",
                                            "/var", "/boot", "/var/lib",     MOCKERHOST_ROOT,
                                            APPS_DIR};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        if (strcmp(path, forbidden[i]) == 0)
            return 1;
    }
    return 0;
}

/* Refuses if the host's mount table has any mount at or below `target`. None are expected
 * (mocker's mounts stay in the container's namespace), but deleting through a mount would
 * delete whatever it exposes, so fail closed. */
static int check_no_mounts(const char *target)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL) {
        perror("rmtree: /proc/self/mountinfo");
        return -1;
    }

    size_t target_len = strlen(target);
    char line[8192];
    int rc = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        /* mountinfo field 5 is the mount point; spaces are octal-escaped, so splitting on
         * whitespace is safe. */
        char mountpoint[PATH_MAX];
        if (sscanf(line, "%*s %*s %*s %*s %4095s", mountpoint) != 1)
            continue;
        if (strncmp(mountpoint, target, target_len) == 0 &&
            (mountpoint[target_len] == '\0' || mountpoint[target_len] == '/')) {
            fprintf(stderr, "rmtree: refusing: '%s' is a mount point at or under '%s' - "
                            "unmount it first\n",
                    mountpoint, target);
            rc = -1;
            break;
        }
    }
    fclose(f);
    return rc;
}

/* Deletes everything inside the directory open as fd (not the directory itself) and takes
 * ownership of fd. A subdirectory on a different device than `dev` aborts. */
static int remove_contents(int fd, dev_t dev, int depth, const char *path)
{
    if (depth > MAX_DEPTH) {
        fprintf(stderr, "rmtree: refusing: nesting deeper than %d at '%s'\n", MAX_DEPTH, path);
        close(fd);
        return -1;
    }

    DIR *d = fdopendir(fd);
    if (d == NULL) {
        fprintf(stderr, "rmtree: fdopendir '%s': %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    int rc = 0;
    for (;;) {
        /* Deleting while iterating can make readdir() skip entries; repeat passes until empty. */
        rewinddir(d);
        int seen = 0;
        struct dirent *e;
        while (rc == 0 && (e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            seen++;

            char child[PATH_MAX];
            int n = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (n < 0 || (size_t)n >= sizeof(child)) {
                fprintf(stderr, "rmtree: path too long under '%s'\n", path);
                rc = -1;
                break;
            }

            struct stat st;
            if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
                fprintf(stderr, "rmtree: lstat '%s': %s\n", child, strerror(errno));
                rc = -1;
                break;
            }

            if (S_ISDIR(st.st_mode)) {
                if (st.st_dev != dev) {
                    fprintf(stderr, "rmtree: refusing: '%s' is on a different device "
                                    "(a mount?)\n",
                            child);
                    rc = -1;
                    break;
                }
                int sub = openat(dirfd(d), e->d_name,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (sub == -1) {
                    fprintf(stderr, "rmtree: open '%s': %s\n", child, strerror(errno));
                    rc = -1;
                    break;
                }
                /* The entry may have been swapped between fstatat() and openat(); the opened
                 * fd must be the inode we looked at. */
                struct stat sst;
                if (fstat(sub, &sst) == -1 || sst.st_dev != dev || sst.st_ino != st.st_ino) {
                    fprintf(stderr, "rmtree: refusing: '%s' changed while being opened\n",
                            child);
                    close(sub);
                    rc = -1;
                    break;
                }
                if (remove_contents(sub, dev, depth + 1, child) == -1) {
                    rc = -1;
                    break;
                }
                if (unlinkat(dirfd(d), e->d_name, AT_REMOVEDIR) == -1) {
                    fprintf(stderr, "rmtree: rmdir '%s': %s\n", child, strerror(errno));
                    rc = -1;
                    break;
                }
                dirs_removed++;
            } else {
                /* Files, symlinks, devices, fifos, sockets: unlink the entry itself; links
                 * are never followed. */
                if (unlinkat(dirfd(d), e->d_name, 0) == -1) {
                    fprintf(stderr, "rmtree: unlink '%s': %s\n", child, strerror(errno));
                    rc = -1;
                    break;
                }
                files_removed++;
            }
        }
        if (rc != 0 || seen == 0)
            break;
    }

    closedir(d); /* also closes fd */
    return rc;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <app-name>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *name = argv[1];
    if (!valid_name(name)) {
        fprintf(stderr, "rmtree: invalid app name (allowed: [a-zA-Z0-9_-], 1-%d chars)\n",
                NAME_MAX_LEN);
        return EXIT_FAILURE;
    }

    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%s/%s", APPS_DIR, name);
    if (path_is_forbidden(target)) {
        fprintf(stderr, "rmtree: refusing: '%s' is a protected path\n", target);
        return EXIT_FAILURE;
    }

    /* apps/ must be a real directory, not a symlink; otherwise apps/<name> could be anywhere. */
    char *real_apps = realpath(APPS_DIR, NULL);
    if (real_apps == NULL) {
        if (errno == ENOENT) {
            printf("rmtree: %s does not exist, nothing to remove\n", APPS_DIR);
            return EXIT_SUCCESS;
        }
        perror("rmtree: realpath " APPS_DIR);
        return EXIT_FAILURE;
    }
    int apps_ok = strcmp(real_apps, APPS_DIR) == 0;
    free(real_apps);
    if (!apps_ok) {
        fprintf(stderr, "rmtree: refusing: %s does not resolve to itself (symlink?)\n",
                APPS_DIR);
        return EXIT_FAILURE;
    }

    int appsfd = open(APPS_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (appsfd == -1) {
        perror("rmtree: open " APPS_DIR);
        return EXIT_FAILURE;
    }
    struct stat apps_st;
    if (fstat(appsfd, &apps_st) == -1) {
        perror("rmtree: fstat " APPS_DIR);
        return EXIT_FAILURE;
    }

    struct stat st;
    if (fstatat(appsfd, name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
        if (errno == ENOENT) {
            printf("rmtree: %s does not exist, nothing to remove\n", target);
            return EXIT_SUCCESS;
        }
        fprintf(stderr, "rmtree: lstat '%s': %s\n", target, strerror(errno));
        return EXIT_FAILURE;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "rmtree: refusing: '%s' is not a real directory (symlink or file)\n",
                target);
        return EXIT_FAILURE;
    }
    if (st.st_dev != apps_st.st_dev) {
        fprintf(stderr, "rmtree: refusing: '%s' is a mount point\n", target);
        return EXIT_FAILURE;
    }

    int rootfd = openat(appsfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootfd == -1) {
        fprintf(stderr, "rmtree: open '%s': %s\n", target, strerror(errno));
        return EXIT_FAILURE;
    }
    struct stat root_st;
    if (fstat(rootfd, &root_st) == -1 || root_st.st_dev != st.st_dev ||
        root_st.st_ino != st.st_ino) {
        fprintf(stderr, "rmtree: refusing: '%s' changed while being opened\n", target);
        return EXIT_FAILURE;
    }

    if (check_no_mounts(target) == -1)
        return EXIT_FAILURE;

    printf("rmtree: removing %s\n", target);
    if (remove_contents(rootfd, root_st.st_dev, 0, target) == -1) {
        fprintf(stderr, "rmtree: aborted - %ld files and %ld directories were removed before "
                        "the error; the rest is left in place, fix the cause and re-run\n",
                files_removed, dirs_removed);
        return EXIT_FAILURE;
    }
    if (unlinkat(appsfd, name, AT_REMOVEDIR) == -1) {
        fprintf(stderr, "rmtree: rmdir '%s': %s\n", target, strerror(errno));
        return EXIT_FAILURE;
    }
    dirs_removed++;

    printf("rmtree: removed %ld files and %ld directories\n", files_removed, dirs_removed);
    return EXIT_SUCCESS;
}
