#ifndef MOUNT_SETUP_H
#define MOUNT_SETUP_H

/* pivot_root()s into rootfs_path and mounts a fresh /proc. Must already be inside its own
 * mount namespace (CLONE_NEWNS). On failure the caller should exit, not undo a partial pivot. */
int mount_setup_pivot_root(const char *rootfs_path);

#endif
