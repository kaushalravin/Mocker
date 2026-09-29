#ifndef UTS_H
#define UTS_H

/*
 * Sets this process's hostname. Must be called from inside a process that
 * already has CLONE_NEWUTS (its own UTS namespace) - it does not create one
 * itself, so this only affects the container, never the host.
 */
int set_container_hostname(const char *hostname);

#endif
