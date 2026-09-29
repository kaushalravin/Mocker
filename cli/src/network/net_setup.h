#ifndef NET_SETUP_H
#define NET_SETUP_H

#include <stddef.h>
#include <sys/types.h>

/*
 * See docs/architecture.md section 7 for the full design. One shared bridge,
 * one /24 subnet - not configurable in v1, same "keep it simple" posture as
 * the rest of the project.
 */
#define NET_BRIDGE_NAME "mocker0"
#define NET_BRIDGE_IP   "10.200.0.1"
#define NET_SUBNET_CIDR "10.200.0.0/24"

/* IFNAMSIZ is 16 (15 usable + NUL) - host/peer veth names are derived from
 * the container name to fit this, not used verbatim (names can be up to 64
 * chars - see net_veth_names()). */
#define NET_IFNAME_MAX 16

/*
 * Ensures the shared "mocker0" bridge exists with NET_BRIDGE_IP, is up, and
 * that IP forwarding + outbound NAT (MASQUERADE) for NET_SUBNET_CIDR are
 * enabled host-wide. Idempotent - safe to call before every `mocker run`,
 * checks before creating/adding rather than assuming a fresh host.
 */
int net_bridge_ensure(void);

/*
 * Allocates the next free IP in NET_SUBNET_CIDR for <name>, recorded in
 * MOCKER_ROOT "/network/leases" (flock()-guarded so concurrent `mocker run`s
 * can't collide). Writes "a.b.c.d" (no /prefix) into ip_out.
 */
int net_alloc_ip(const char *name, char *ip_out, size_t ip_out_size);

/*
 * Derives this container's host-side and peer veth interface names from
 * <name>, bounded to NET_IFNAME_MAX. Both buffers must be at least
 * NET_IFNAME_MAX bytes.
 */
void net_veth_names(const char *name, char *host_veth_out, char *peer_veth_out);

/*
 * Host-side veth wiring, run from the parent right after clone() returns
 * (same "attach before the container does anything expensive" timing as
 * cgroup_attach() - docs/architecture.md section 7): creates the veth pair,
 * moves peer_veth into pid's network namespace, attaches host_veth to the
 * bridge, and brings host_veth up.
 */
int net_veth_setup(const char *host_veth, const char *peer_veth, pid_t pid);

/*
 * Container-side config, run from inside the child before pivot_root (see
 * docs/architecture.md section 7 for why pre-pivot_root matters here):
 * renames peer_veth to "eth0" inside the child's own (already-created)
 * network namespace, assigns ip/24, brings up eth0 and lo, and adds a
 * default route via the bridge gateway.
 */
int net_container_configure(const char *peer_veth, const char *ip);

/*
 * Adds PREROUTING + OUTPUT DNAT rules (both are needed - PREROUTING alone
 * misses traffic the host itself originates, e.g. `curl localhost:<port>`)
 * plus a FORWARD accept rule, mapping host_port to container_ip:container_port.
 */
int net_port_map(const char *container_ip, int host_port, int container_port);

/*
 * `mocker rm`'s network cleanup (docs/architecture.md section 5) - the
 * release/delete counterparts to net_alloc_ip()/net_veth_setup()/net_port_map()
 * above. Each is idempotent: safe to call on state that's already gone
 * (nothing leased, interface already deleted, rule already absent), since rm
 * should still make forward progress on a container whose networking was
 * only partly set up (e.g. `run` died before finishing).
 */

/* Removes <name>'s line from MOCKER_ROOT "/network/leases", same
 * flock()-guarded read-modify-write pattern as net_alloc_ip(). */
int net_release_ip(const char *name);

/* `ip link delete host_veth`. The peer end doesn't need a separate delete -
 * it lives inside the container's network namespace, which the kernel tears
 * down automatically once that namespace's last process exits. */
int net_veth_delete(const char *host_veth);

/* Removes the exact PREROUTING/OUTPUT/FORWARD rules net_port_map() added,
 * via `-D` with identical arguments. Only meaningful for a container that
 * was actually started with --port - the caller checks that first. */
int net_port_unmap(const char *container_ip, int host_port, int container_port);

#endif
