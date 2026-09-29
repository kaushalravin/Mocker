#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <sys/types.h>

/*
 * Creates the container's PID 1 with one clone() call carrying
 * CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET together - see
 * docs/architecture.md section 2 ("Why the current prototype's shape has to
 * change") for why this must be one call, not namespaces layered on via
 * separate fork()/unshare()/clone() steps.
 *
 * Inside the new namespaces, the child first blocks until the parent signals
 * (via the pipe this function sets up - see docs/architecture.md section 7
 * "Synchronization") that peer_veth has been moved into the child's own new
 * network namespace; only then does it rename peer_veth to "eth0" and assign
 * container_ip/24 (net_container_configure()). It then sets its hostname,
 * pivot_root()s into rootfs_path, mounts a fresh /proc, and execvp()s argv.
 * argv must be NULL-terminated; argv[0] is the command to run.
 *
 * On success, *net_ready_fd_out receives the write end of that pipe - the
 * caller must write one byte to it (after moving peer_veth into the child's
 * netns and attaching the host end to the bridge) to release the child, then
 * close it.
 *
 * Returns the child's host-visible PID (for the caller's waitpid()), or -1.
 */
pid_t launch_container(const char *rootfs_path, const char *hostname, char *const argv[],
                        const char *peer_veth, const char *container_ip, int *net_ready_fd_out);

#endif
