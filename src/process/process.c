#define _GNU_SOURCE

#include "process.h"
#include "namespace/namespace.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>

pid_t create_process()
{
    int child_pid = fork();

    if (child_pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    } else if (child_pid == 0) {
        /* setting up all the namespaces to the child process */
        setup_namespaces();
        _exit(0);
    }

    return child_pid;
}

void wait_for_process(pid_t pid) {
    printf("PARENT: waiting for namespace manager...\n");
    fflush(stdout);

    int status;

    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        exit(EXIT_FAILURE);
    }

    printf("PARENT: namespace manager exited.\n");
    fflush(stdout);
}

