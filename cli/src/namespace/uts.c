#define _GNU_SOURCE

#include "uts.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int set_container_hostname(const char *hostname)
{
    /* No unshare(): clone() already gave us a fresh UTS namespace, so this can't touch the host. */
    if (sethostname(hostname, strlen(hostname)) == -1) {
        perror("sethostname");
        return -1;
    }

    printf("UTS: hostname set to %s\n", hostname);
    return 0;
}
