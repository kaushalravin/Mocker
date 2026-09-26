#ifndef PROCESS_H
#define PROCESS_H

#include <sys/types.h>

#include <stdio.h>      // perror()
#include <stdlib.h>     // exit()
#include <unistd.h>     // fork(), execlp()
#include <sys/types.h>  // pid_t
#include <sys/wait.h>   // waitpid()

pid_t create_process();
void wait_for_process(pid_t pid);

#endif