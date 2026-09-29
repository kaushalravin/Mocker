#ifndef PROCESS_H
#define PROCESS_H

#include <sys/types.h>  // pid_t

pid_t create_process(const char *rootfs_path, char *const argv[]);
void wait_for_process(pid_t pid);

#endif
