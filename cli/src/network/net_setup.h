#ifndef NET_SETUP_H
#define NET_SETUP_H

#include <stddef.h>
#include <sys/types.h>

/* One shared bridge and one /24 subnet (docs/architecture.md section 7). */
#define NET_BRIDGE_NAME "mocker0"
#define NET_BRIDGE_IP   "10.200.0.1"
#define NET_SUBNET_CIDR "10.200.0.0/24"

/* IFNAMSIZ: veth names are derived from the container name to fit this. */
#define NET_IFNAME_MAX 16

/* Idempotent: creates mocker0, enables IP forwarding and MASQUERADE if missing. */
int net_bridge_ensure(void);

/* Leases the next free IP ("a.b.c.d") for <name>; flock()-guarded against concurrent runs. */
int net_alloc_ip(const char *name, char *ip_out, size_t ip_out_size);

/* Derives the host-side and peer veth names from <name>; buffers need NET_IFNAME_MAX bytes. */
void net_veth_names(const char *name, char *host_veth_out, char *peer_veth_out);

/* Parent side, right after clone(): create the veth pair, move the peer into pid's netns,
 * attach the host end to the bridge. */
int net_veth_setup(const char *host_veth, const char *peer_veth, pid_t pid);

/* Child side, before pivot_root(): rename the peer to eth0, set ip/24, bring up eth0 and lo,
 * add the default route via the bridge. */
int net_container_configure(const char *peer_veth, const char *ip);

/* PREROUTING + OUTPUT DNAT and a FORWARD accept (OUTPUT covers host-originated traffic). */
int net_port_map(const char *container_ip, int host_port, int container_port);

/* The functions below are `mocker rm`'s cleanup; each is idempotent so rm can finish a
 * partly set-up container. */

/* Removes <name>'s lease line. */
int net_release_ip(const char *name);

/* Deletes the host veth; the peer goes away with the container's netns. */
int net_veth_delete(const char *host_veth);

/* Removes the exact rules net_port_map() added. */
int net_port_unmap(const char *container_ip, int host_port, int container_port);

#endif
