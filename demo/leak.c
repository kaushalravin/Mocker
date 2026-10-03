#define _GNU_SOURCE

/* Deliberate memory leak: allocates and touches 1 MB at a time until memory.max gets it
 * OOM-killed. The proof is on the host (memory.events oom_kill, kernel log), not this output.
 * Stops itself at LEAK_SAFETY_CAP_MB so a mistaken host run can't eat the VM's RAM; reaching
 * the cap means no limit worked. */

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
