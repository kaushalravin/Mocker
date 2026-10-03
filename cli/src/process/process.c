#define _GNU_SOURCE

#include "process.h"
#include "namespace/namespace.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

pid_t create_process(const char *rootfs_path, const char *hostname, char *const argv[],
                      const char *peer_veth, const char *container_ip, int *net_ready_fd_out)
{
    /* No fork(): launch_container()'s single clone() is the process creation. */
    pid_t pid = launch_container(rootfs_path, hostname, argv, peer_veth, container_ip,
                                  net_ready_fd_out);
    if (pid == -1)
        exit(EXIT_FAILURE);

    return pid;
}

void wait_for_process(pid_t pid)
{
    printf("MOCKER: waiting for container PID 1 (host PID %d)...\n", pid);
    fflush(stdout);

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        exit(EXIT_FAILURE);
    }

    printf("MOCKER: container exited.\n");
}
