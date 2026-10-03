#define _GNU_SOURCE

/* Deliberate fork-until-refused: children just sleep, so pids.max makes fork() fail with
 * EAGAIN. The proof is pids.events "max" on the host. Linear on purpose (children don't fork
 * again), capped at FORK_SAFETY_CAP, and each child exits after CHILD_LIFETIME_SEC, so a
 * mistaken host run is harmless. */

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

    /* Reap the children so the container exits without zombies. */
    while (wait(NULL) > 0)
        ;
    return children == FORK_SAFETY_CAP ? EXIT_FAILURE : EXIT_SUCCESS;
}
