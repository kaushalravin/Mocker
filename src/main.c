#include <stdio.h>
#include "process/process.h"

int main()
{
    printf("MINIDOCKER: starting...\n");
    fflush(stdout);

    pid_t pid = create_process();

    printf("MINIDOCKER: child PID = %d\n", pid);
    fflush(stdout);

    wait_for_process(pid);

    printf("MINIDOCKER: container finished.\n");

    return 0;
}