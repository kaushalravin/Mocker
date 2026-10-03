#ifndef UTS_H
#define UTS_H

/* Sets the hostname; only affects the container because the caller is in its own UTS namespace. */
int set_container_hostname(const char *hostname);

#endif
