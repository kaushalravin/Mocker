#define _GNU_SOURCE

/*
 * Deliberate fork-until-refused, to prove pids.max is enforced by the kernel.
 *
 * Keeps calling fork(); every child just sleeps. Inside a container started
 * with `--pids <n>`, the cgroup's pids.max makes fork() fail with EAGAIN once
 * the group holds n processes, and this program reports how many children it
 * got before that happened. The kernel's own evidence is on the host:
 * pids.events's "max" counter counts the forks it refused - see demo/README.md.
 *
 * This is a linear fork loop, not a true exponential bomb (children don't
 * fork again). That's deliberate: it exercises exactly the same limit, but
 * can't take down the VM if it's run on the host by mistake - it stops at
 * FORK_SAFETY_CAP children, and every child exits by itself after
 * CHILD_LIFETIME_SEC via alarm(). If the cap is reached, no limit stopped it,
 * and the message says so.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define FORK_SAFETY_CAP    500
#define CHILD_LIFETIME_SEC 20

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("forkbomb: pid %d, forking until refused (safety cap %d children)\n", (int)getpid(),
           FORK_SAFETY_CAP);

    int children = 0;
    for (; children < FORK_SAFETY_CAP; children++) {
        pid_t pid = fork();
        if (pid == -1) {
            printf("forkbomb: fork() refused after %d children: %s (errno %d)\n", children,
                   strerror(errno), errno);
            break;
        }
        if (pid == 0) {
            alarm(CHILD_LIFETIME_SEC); /* SIGALRM's default action ends the child */
            for (;;)
                pause();
        }
    }

    if (children == FORK_SAFETY_CAP)
        printf("forkbomb: reached the %d-child safety cap and fork() was NEVER refused - "
               "no pids limit stopped this process\n",
               FORK_SAFETY_CAP);

    printf("forkbomb: holding %d children for %d s so pids.current can be inspected...\n",
           children, CHILD_LIFETIME_SEC);
    sleep(CHILD_LIFETIME_SEC);

    /* Reap the children so the container exits cleanly instead of leaving
     * zombies behind. They've either expired by now or are about to. */
    while (wait(NULL) > 0)
        ;
    return children == FORK_SAFETY_CAP ? EXIT_FAILURE : EXIT_SUCCESS;
}
