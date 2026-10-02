#define _GNU_SOURCE

/*
 * Deliberate memory leak, to prove memory.max is enforced by the kernel.
 *
 * Allocates 1 MB at a time and touches every page, so the memory is actually
 * resident (an untouched malloc() isn't charged to the cgroup yet). It never
 * frees anything. Inside a container started with `--mem-mb <n>`, the cgroup's
 * memory.max is the wall: once the group is over it and nothing can be
 * reclaimed (mocker also sets memory.swap.max=0), the kernel's cgroup OOM
 * killer SIGKILLs a process in the group - this one.
 *
 * It prints its own progress, but that's just narration. The proof is on the
 * host: memory.events (oom_kill) and the kernel log - see demo/README.md.
 *
 * Bounded on purpose: it stops itself at LEAK_SAFETY_CAP_MB so that running it
 * on the host by mistake can't eat all of the VM's RAM. Exiting on its own
 * means no limit stopped it, which the message says out loud.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LEAK_SAFETY_CAP_MB 512

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("leak: pid %d, leaking 1 MB every 50 ms (safety cap %d MB)\n", (int)getpid(),
           LEAK_SAFETY_CAP_MB);

    for (int mb = 1; mb <= LEAK_SAFETY_CAP_MB; mb++) {
        char *block = malloc(1024 * 1024);
        if (block == NULL) {
            printf("leak: malloc failed at %d MB (address-space limit, not the cgroup)\n", mb);
            return EXIT_FAILURE;
        }
        memset(block, 1, 1024 * 1024); /* touch every page so it's really resident */
        printf("leak: holding %d MB\n", mb);
        usleep(50 * 1000);
    }

    printf("leak: reached the %d MB safety cap and was NOT killed - no memory limit "
           "stopped this process\n",
           LEAK_SAFETY_CAP_MB);
    return EXIT_SUCCESS;
}
