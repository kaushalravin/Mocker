#include <stdio.h>
#include "process/process.h"

int main()
{
    printf("MOCKER: starting...\n");
    fflush(stdout);

    pid_t pid = create_process();

    printf("MOCKER: child PID = %d\n", pid);
    fflush(stdout);

    wait_for_process(pid);

    printf("MOCKER: container finished.\n");

    return 0;
}