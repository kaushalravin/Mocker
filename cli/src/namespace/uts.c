#define _GNU_SOURCE

#include "uts.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int set_container_hostname(const char *hostname)
{
    /* No unshare(CLONE_NEWUTS) here - the single clone() call in
     * namespace.c already put this process in a fresh UTS namespace, so
     * sethostname() only ever affects the container, never the host. */
    if (sethostname(hostname, strlen(hostname)) == -1) {
        perror("sethostname");
        return -1;
    }

    printf("UTS: hostname set to %s\n", hostname);
    return 0;
}
