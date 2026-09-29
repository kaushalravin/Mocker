#define _GNU_SOURCE

#include "mount_setup.h"

#include <limits.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * pivot_root() swaps a process's whole mount tree root - unlike chroot(),
 * which only changes the apparent root directory and leaves the old root
 * reachable through it. glibc doesn't reliably wrap this syscall, so it's
 * invoked directly. See docs/architecture.md section 4 for why each step
 * below is in this exact order.
 */
static int do_pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

int mount_setup_pivot_root(const char *rootfs_path)
{
    char old_root[PATH_MAX];

    /* Step 1: detach this mount namespace from host propagation *before*
     * touching anything else. MS_REC so it applies to every mount already
     * under "/", not just the top-level one. After this, nothing the
     * container does to its mounts can leak to the host, and nothing the
     * host does can leak in. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("mount_setup: MS_PRIVATE remount");
        return -1;
    }

    /* Step 2: pivot_root() requires new_root to be an actual mount point,
     * not just a directory - bind-mounting it onto itself makes it one
     * without needing a second filesystem. */
    if (mount(rootfs_path, rootfs_path, NULL, MS_BIND, NULL) == -1) {
        perror("mount_setup: bind mount rootfs onto itself");
        return -1;
    }

    int n = snprintf(old_root, sizeof(old_root), "%s/.old_root", rootfs_path);
    if (n < 0 || (size_t)n >= sizeof(old_root)) {
        fprintf(stderr, "mount_setup: rootfs path too long: %s\n", rootfs_path);
        return -1;
    }

    /* Step 3: pivot_root's put_old directory must exist inside new_root. */
    if (mkdir(old_root, 0700) == -1) {
        perror(old_root);
        return -1;
    }

    /* Step 4-5: pivot_root(".", ".old_root") is called with the process's
     * cwd as new_root, which is why we chdir() into it first. */
    if (chdir(rootfs_path) == -1) {
        perror("mount_setup: chdir(rootfs_path)");
        return -1;
    }

    if (do_pivot_root(".", ".old_root") == -1) {
        perror("mount_setup: pivot_root");
        return -1;
    }

    /* Step 6: re-anchor into the new root now that "/" has changed under us. */
    if (chdir("/") == -1) {
        perror("mount_setup: chdir(\"/\") after pivot_root");
        return -1;
    }

    /* Step 7: fresh /proc for the new PID namespace - without this, /proc
     * still shows whatever the old root's /proc pointed at (typically
     * nothing usable, or the host's, depending on what was bind-mounted). */
    if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
        perror("mount_setup: mount fresh /proc");
        return -1;
    }

    /* Step 8-9: lazily detach the old root from *this* mount namespace only
     * - MNT_DETACH means it disappears once nothing still references it.
     * This never touches the host's real root; it only removes it from the
     * container's now-private mount table. */
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
