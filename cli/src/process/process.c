#define _GNU_SOURCE

#include "process.h"
#include "namespace/namespace.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

pid_t create_process(const char *rootfs_path, char *const argv[])
{
    /* No fork() here - launch_container()'s single clone() call *is* the
     * process creation step. Forking first and setting up namespaces inside
     * the fork is exactly the piecemeal pattern stage 3 replaced (see
     * docs/architecture.md section 2). */
    pid_t pid = launch_container(rootfs_path, argv);
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
