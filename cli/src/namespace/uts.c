#define _GNU_SOURCE

#include "uts.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>

void setup_uts_namespace(){

    if(unshare(CLONE_NEWUTS) == -1)
    {
        perror("unshare UTS");
        exit(EXIT_FAILURE);
    }

    if (sethostname("mocker", 6) == -1)
    {
        perror("sethostname");
        exit(EXIT_FAILURE);
    }

    printf("UTS: hostname set to mocker\n");
}