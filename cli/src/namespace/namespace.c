#define _GNU_SOURCE

#include "namespace.h"
#include "uts.h"
#include "../mount/mount_setup.h"

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CLONE_STACK_SIZE (1024 * 1024)

/* clone()'s raw void *arg can only carry one pointer, so the child's real
 * inputs (rootfs path, hostname, command) travel together in this struct. */
typedef struct {
    const char *rootfs_path;
    const char *hostname;
    char *const *argv;
} container_args_t;

/* Runs as PID 1 inside the fresh PID/mount/UTS namespaces created by the
 * clone() call below. */
static int container_entry(void *arg)
{
    container_args_t *args = (container_args_t *)arg;

    printf("CONTAINER: PID 1 alive (getpid()=%d inside its own namespace)\n", getpid());
    fflush(stdout);

    if (set_container_hostname(args->hostname) == -1)
        _exit(EXIT_FAILURE);

    if (mount_setup_pivot_root(args->rootfs_path) == -1)
        _exit(EXIT_FAILURE);

    execvp(args->argv[0], args->argv);

    /* Only reached if execvp() failed. */
    perror("execvp");
    _exit(EXIT_FAILURE);
}

pid_t launch_container(const char *rootfs_path, const char *hostname, char *const argv[])
{
    void *stack = malloc(CLONE_STACK_SIZE);
    if (stack == NULL) {
        perror("malloc clone stack");
        return -1;
    }

    void *stack_top = (char *)stack + CLONE_STACK_SIZE;

    container_args_t args = {
        .rootfs_path = rootfs_path,
        .hostname = hostname,
        .argv = argv,
    };

    /* One clone() call, one flag set - PID/mount/UTS namespaces are created
     * together at this single boundary, not layered on across separate
     * fork()/unshare()/clone() calls. This matters because the pivot_root()
     * sequence in mount_setup.c needs both the new PID namespace (so PID 1
     * inside it is the container's init) and the new mount namespace (so the
     * pivot only ever affects this process's view) to already exist at the
     * moment it runs. */
    pid_t pid = clone(container_entry, stack_top,
                       CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                       &args);

    if (pid == -1) {
        perror("clone");
        free(stack);
        return -1;
    }

    /* Safe to free here even though the child is still running: clone() was
     * called without CLONE_VM, so the child got its own copy-on-write
     * address space (same semantics as fork()) - this free() only affects
     * the parent's mapping, not whatever the child is still executing on. */
    free(stack);

    return pid;
}
