#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <sys/types.h>

/*
 * Creates the container's PID 1 with one clone() call carrying
 * CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS together - see
 * docs/architecture.md section 2 ("Why the current prototype's shape has to
 * change") for why this must be one call, not namespaces layered on via
 * separate fork()/unshare()/clone() steps.
 *
 * Inside the new namespaces, the child sets its hostname, pivot_root()s into
 * rootfs_path, mounts a fresh /proc, then execvp()s argv. argv must be
 * NULL-terminated; argv[0] is the command to run.
 *
 * Returns the child's host-visible PID (for the caller's waitpid()), or -1.
 */
pid_t launch_container(const char *rootfs_path, char *const argv[]);

#endif
