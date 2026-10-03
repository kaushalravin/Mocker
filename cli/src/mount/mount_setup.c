#define _GNU_SOURCE

#include "mount_setup.h"

#include <limits.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* pivot_root() swaps the whole mount root (chroot() leaves the old root reachable). glibc has
 * no wrapper. Step order is explained in docs/architecture.md section 4. */
static int do_pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

int mount_setup_pivot_root(const char *rootfs_path)
{
    char old_root[PATH_MAX];

    /* 1. Stop mount events propagating between host and container (MS_REC: existing mounts too). */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("mount_setup: MS_PRIVATE remount");
        return -1;
    }

    /* 2. new_root must be a mount point; bind-mount it onto itself. */
    if (mount(rootfs_path, rootfs_path, NULL, MS_BIND, NULL) == -1) {
        perror("mount_setup: bind mount rootfs onto itself");
        return -1;
    }

    int n = snprintf(old_root, sizeof(old_root), "%s/.old_root", rootfs_path);
    if (n < 0 || (size_t)n >= sizeof(old_root)) {
        fprintf(stderr, "mount_setup: rootfs path too long: %s\n", rootfs_path);
        return -1;
    }

    /* 3. put_old must exist inside new_root. */
    if (mkdir(old_root, 0700) == -1) {
        perror(old_root);
        return -1;
    }

    /* 4-5. pivot_root(".", ".old_root") treats the cwd as new_root. */
    if (chdir(rootfs_path) == -1) {
        perror("mount_setup: chdir(rootfs_path)");
        return -1;
    }

    if (do_pivot_root(".", ".old_root") == -1) {
        perror("mount_setup: pivot_root");
        return -1;
    }

    /* 6. "/" changed under us; re-anchor. */
    if (chdir("/") == -1) {
        perror("mount_setup: chdir(\"/\") after pivot_root");
        return -1;
    }

    /* 7. Fresh /proc for the new PID namespace. */
    if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
        perror("mount_setup: mount fresh /proc");
        return -1;
    }

    /* 8-9. Lazily detach the old root from this private mount namespace; the host is untouched. */
    if (umount2("/.old_root", MNT_DETACH) == -1) {
        perror("mount_setup: umount2(/.old_root)");
        return -1;
    }

    if (rmdir("/.old_root") == -1) {
        perror("mount_setup: rmdir(/.old_root)");
        return -1;
    }

    return 0;
}
