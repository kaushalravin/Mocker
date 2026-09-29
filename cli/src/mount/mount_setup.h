#ifndef MOUNT_SETUP_H
#define MOUNT_SETUP_H

/*
 * Swaps the calling process's mount tree root onto rootfs_path via
 * pivot_root(), and mounts a fresh /proc for the new PID namespace.
 *
 * Must be called from inside a process that already has CLONE_NEWNS (its own
 * mount namespace) - it does not create one itself. rootfs_path must already
 * exist and be populated (see docs/architecture.md "Rootfs / mount lifecycle").
 *
 * On success, "/" in this process is rootfs_path and the old host root is
 * gone from this mount namespace. On failure, returns -1 and the caller
 * should give up (exit), not attempt to undo a partially-completed pivot.
 */
int mount_setup_pivot_root(const char *rootfs_path);

#endif
