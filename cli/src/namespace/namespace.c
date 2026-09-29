#define _GNU_SOURCE

#include "namespace.h"
#include "uts.h"
#include "../mount/mount_setup.h"
#include "../network/net_setup.h"

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CLONE_STACK_SIZE (1024 * 1024)

/* clone()'s raw void *arg can only carry one pointer, so the child's real
 * inputs (rootfs path, hostname, command, network handshake) travel together
 * in this struct. */
typedef struct {
    const char *rootfs_path;
    const char *hostname;
    char *const *argv;
    const char *peer_veth;
    const char *container_ip;
    int net_ready_fd; /* read end - see docs/architecture.md section 7 */
} container_args_t;

/* Runs as PID 1 inside the fresh PID/mount/UTS/net namespaces created by the
 * clone() call below. */
static int container_entry(void *arg)
{
    container_args_t *args = (container_args_t *)arg;

    printf("CONTAINER: PID 1 alive (getpid()=%d inside its own namespace)\n", getpid());
    fflush(stdout);

    /* Block until the parent has moved peer_veth into this process's own
     * (already-created, at clone() time) network namespace and attached the
     * host end to the bridge - only then does an interface named peer_veth
     * actually exist in here to rename/configure. See docs/architecture.md
     * section 7 "Synchronization". */
    char ready;
    if (read(args->net_ready_fd, &ready, 1) != 1) {
        fprintf(stderr, "container: did not receive network-ready signal from parent\n");
        _exit(EXIT_FAILURE);
    }
    close(args->net_ready_fd);

    /* Runs before pivot_root deliberately: this process's mount namespace is
     * still a private copy of the host's mount table at this point (only
     * pivot_root switches it - see docs/architecture.md section 4), so
     * execvp("ip", ...) here still finds the host's real iproute2 even
     * though the eventual container rootfs has none of its own. */
    if (net_container_configure(args->peer_veth, args->container_ip) == -1)
        _exit(EXIT_FAILURE);

    if (set_container_hostname(args->hostname) == -1)
        _exit(EXIT_FAILURE);

    if (mount_setup_pivot_root(args->rootfs_path) == -1)
        _exit(EXIT_FAILURE);

    execvp(args->argv[0], args->argv);

    /* Only reached if execvp() failed. */
    perror("execvp");
    _exit(EXIT_FAILURE);
}

pid_t launch_container(const char *rootfs_path, const char *hostname, char *const argv[],
                        const char *peer_veth, const char *container_ip, int *net_ready_fd_out)
{
    void *stack = malloc(CLONE_STACK_SIZE);
    if (stack == NULL) {
        perror("malloc clone stack");
        return -1;
    }

    void *stack_top = (char *)stack + CLONE_STACK_SIZE;

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        free(stack);
        return -1;
    }

    container_args_t args = {
        .rootfs_path = rootfs_path,
        .hostname = hostname,
        .argv = argv,
        .peer_veth = peer_veth,
        .container_ip = container_ip,
        .net_ready_fd = pipefd[0],
    };

    /* One clone() call, one flag set - PID/mount/UTS/net namespaces are
     * created together at this single boundary, not layered on across
     * separate fork()/unshare()/clone() calls. This matters because the
     * pivot_root() sequence in mount_setup.c needs both the new PID
     * namespace (so PID 1 inside it is the container's init) and the new
     * mount namespace (so the pivot only ever affects this process's view)
     * to already exist at the moment it runs; CLONE_NEWNET joins the same
     * boundary at stage 5 for the identical reason - the container's network
     * namespace has to exist before anything inside tries to configure it. */
    pid_t pid = clone(container_entry, stack_top,
                       CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | SIGCHLD,
                       &args);

    /* Safe to free here even though the child is still running: clone() was
     * called without CLONE_VM, so the child got its own copy-on-write
     * address space (same semantics as fork()) - this free() only affects
     * the parent's mapping, not whatever the child is still executing on. */
    free(stack);

    if (pid == -1) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* Only the child reads from this pipe. */
    close(pipefd[0]);
    *net_ready_fd_out = pipefd[1];

    return pid;
}
