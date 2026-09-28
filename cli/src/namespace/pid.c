#define _GNU_SOURCE

#include "pid.h"

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int pid_ns_child(void *arg)
{
    (void)arg;

    printf("PID: inside PID namespace\n");
    printf("PID: my PID = %d\n", getpid());
    fflush(stdout);

    /*
     * This process is PID 1 inside
     * the new PID namespace.
     */

    char *const argv[] = {
        "/bin/bash",
        "-i",
        NULL
    };

    execv("/bin/bash", argv);

    perror("execv bash");
    _exit(EXIT_FAILURE);
}


void setup_pid_namespace(void)
{
    printf("PID: setting up PID namespace...\n");
    fflush(stdout);

    /*
     * clone() creates a child process
     * inside a new PID namespace.
     */

    size_t stack_size = 65536;

    void *stack = malloc(stack_size);

    if (stack == NULL)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    void *stack_top = (char *)stack + stack_size;

    pid_t pid = clone(
        pid_ns_child,
        stack_top,
        CLONE_NEWPID | SIGCHLD,
        NULL
    );

    if (pid == -1)
    {
        perror("clone PID namespace");
        free(stack);
        exit(EXIT_FAILURE);
    }

    printf(
        "PID: namespace child created. Host PID = %d\n",
        pid
    );

    fflush(stdout);

    /*
     * Parent waits for Bash/container process.
     */

    int status;

    if (waitpid(pid, &status, 0) == -1)
    {
        perror("waitpid");
        free(stack);
        exit(EXIT_FAILURE);
    }

    free(stack);

    printf("PID: container process exited.\n");
}