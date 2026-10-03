#ifndef PROCESS_H
#define PROCESS_H

#include <sys/types.h>

pid_t create_process(const char *rootfs_path, const char *hostname, char *const argv[],
                      const char *peer_veth, const char *container_ip, int *net_ready_fd_out);
void wait_for_process(pid_t pid);

#endif
