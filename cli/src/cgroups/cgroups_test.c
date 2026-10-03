#define _GNU_SOURCE

/* Standalone stage-2 proof: a memory-leaking child in a 20MB cgroup must be OOM-killed by the
 * kernel, judged by memory.current alone. Run as root on Linux with cgroups v2. */

#include "cgroups.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_CGROUP_NAME "cgroup-test"
#define TEST_MEM_LIMIT_BYTES (20L * 1024 * 1024)
#define TEST_PIDS_LIMIT 20

/* Waits for the parent's go-ahead (cgroup attached), then leaks memory. */
static void child_leak_memory(int sync_fd)
{
    char byte;
    read(sync_fd, &byte, 1);
    close(sync_fd);

    for (;;) {
        char *block = malloc(1024 * 1024);
        if (block == NULL)
            _exit(EXIT_FAILURE); /* shouldn't happen - the OOM-kill should land first */
        memset(block, 1, 1024 * 1024); /* touch every page so it's actually resident */
        usleep(100 * 1000);
    }
}

int main(void)
{
    if (!cgroups_v2_available()) {
        fprintf(stderr, "cgroups v2 not available - run this on the Lubuntu VM.\n");
        return EXIT_FAILURE;
    }

    if (cgroup_create(TEST_CGROUP_NAME) == -1)
        return EXIT_FAILURE;

    cgroup_limits_t limits = {
        .mem_max_bytes = TEST_MEM_LIMIT_BYTES,
        .pids_max = TEST_PIDS_LIMIT,
        .cpu_max_percent = -1,
    };

    if (cgroup_apply_limits(TEST_CGROUP_NAME, &limits) == -1) {
        cgroup_remove(TEST_CGROUP_NAME);
        return EXIT_FAILURE;
    }

    printf("cgroup '%s' ready: memory.max=%ldMB pids.max=%d\n",
           TEST_CGROUP_NAME, TEST_MEM_LIMIT_BYTES / (1024 * 1024), TEST_PIDS_LIMIT);

    int sync_pipe[2];
    if (pipe(sync_pipe) == -1) {
        perror("pipe");
        cgroup_remove(TEST_CGROUP_NAME);
        return EXIT_FAILURE;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        cgroup_remove(TEST_CGROUP_NAME);
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        close(sync_pipe[1]);
        child_leak_memory(sync_pipe[0]);
        _exit(EXIT_FAILURE); /* unreachable */
    }

    close(sync_pipe[0]);

    /* Attach before the child runs so it is never unconstrained. */
    if (cgroup_attach(TEST_CGROUP_NAME, pid) == -1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        cgroup_remove(TEST_CGROUP_NAME);
        return EXIT_FAILURE;
    }
    write(sync_pipe[1], "x", 1);
    close(sync_pipe[1]);

    printf("watching memory.current (kernel-reported, not the child's own count)...\n");

    int status;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        long current = cgroup_get_memory_current(TEST_CGROUP_NAME);
        if (current >= 0)
            printf("memory.current = %ld MB\n", current / (1024 * 1024));
        usleep(200 * 1000);
    }

    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
        printf("PASS: kernel OOM-killed the child at the cgroup memory limit.\n");
    else
        printf("FAIL: child was not OOM-killed (status=%d).\n", status);

    cgroup_remove(TEST_CGROUP_NAME);
    return 0;
}
