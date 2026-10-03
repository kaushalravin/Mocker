#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <sys/types.h>

/* Creates the container's PID 1 with one clone(CLONE_NEWPID|NEWNS|NEWUTS|NEWNET) call.
 * The child blocks on a pipe until the parent has moved peer_veth into its netns, then
 * configures eth0, sets the hostname, pivot_root()s into rootfs_path, mounts /proc and
 * execvp()s argv (NULL-terminated).
 *
 * *net_ready_fd_out receives the pipe's write end: the caller writes one byte to release
 * the child, then closes it. Returns the child's host PID, or -1. */
pid_t launch_container(const char *rootfs_path, const char *hostname, char *const argv[],
                        const char *peer_veth, const char *container_ip, int *net_ready_fd_out);

#endif
