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

/* clone() passes one pointer, so the child's inputs travel in a struct. */
typedef struct {
    const char *rootfs_path;
    const char *hostname;
    char *const *argv;
    const char *peer_veth;
    const char *container_ip;
    int net_ready_fd; /* read end of the network handshake pipe */
} container_args_t;

/* Runs as PID 1 inside the new namespaces. */
static int container_entry(void *arg)
{
    container_args_t *args = (container_args_t *)arg;

    printf("CONTAINER: PID 1 alive (getpid()=%d inside its own namespace)\n", getpid());
    fflush(stdout);

    /* Wait until the parent has moved peer_veth into our netns (docs/architecture.md section 7). */
    char ready;
    if (read(args->net_ready_fd, &ready, 1) != 1) {
        fprintf(stderr, "container: did not receive network-ready signal from parent\n");
        _exit(EXIT_FAILURE);
    }
    close(args->net_ready_fd);

    /* Before pivot_root on purpose: the host's ip/iptables are still reachable here. */
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

    /* One clone() creates every namespace together; pivot_root() and the network
     * config both need them to exist already. */
    pid_t pid = clone(container_entry, stack_top,
                       CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET | SIGCHLD,
                       &args);

    /* Safe to free: no CLONE_VM, so the child has its own copy-on-write address space. */
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
