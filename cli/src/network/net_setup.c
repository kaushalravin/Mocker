#define _GNU_SOURCE

#include "net_setup.h"
#include "../registry/registry.h" /* MOCKER_ROOT */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NET_LEASES_DIR  MOCKER_ROOT "/network"
#define NET_LEASES_FILE NET_LEASES_DIR "/leases"

/* Runs argv[0] with argv and waits. Never system()/popen(): arguments are literals or validated
 * values in a real argv array, so there is no shell to inject into. */
static int net_run(char *const argv[])
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "net: '%s' failed\n", argv[0]);
        return -1;
    }

    return 0;
}

/* Like net_run() for existence checks (e.g. `iptables -C`): non-zero just means "not present".
 * Returns 1 if exit 0, 0 if not, -1 on a real error. */
static int net_check(char *const argv[])
{
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid");
        return -1;
    }

    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status) == 0 ? 1 : 0;
}

int net_bridge_ensure(void)
{
    if (if_nametoindex(NET_BRIDGE_NAME) == 0) {
        char *add_argv[] = {"ip", "link", "add", NET_BRIDGE_NAME, "type", "bridge", NULL};
        if (net_run(add_argv) == -1)
            return -1;

        char *addr_argv[] = {"ip", "addr", "add", NET_BRIDGE_IP "/24", "dev", NET_BRIDGE_NAME,
                              NULL};
        if (net_run(addr_argv) == -1)
            return -1;

        char *up_argv[] = {"ip", "link", "set", NET_BRIDGE_NAME, "up", NULL};
        if (net_run(up_argv) == -1)
            return -1;
    }

    /* Host-wide; a direct file write rather than a shelled-out sysctl. */
    int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
    if (fd == -1) {
        perror("/proc/sys/net/ipv4/ip_forward");
        return -1;
    }
    if (write(fd, "1\n", 2) != 2) {
        perror("/proc/sys/net/ipv4/ip_forward");
        close(fd);
        return -1;
    }
    close(fd);

    char *check_argv[] = {"iptables", "-t", "nat", "-C", "POSTROUTING",
                           "-s",       NET_SUBNET_CIDR,
                           "!",        "-o", NET_BRIDGE_NAME,
                           "-j",       "MASQUERADE", NULL};
    int exists = net_check(check_argv);
    if (exists == -1)
        return -1;

    if (!exists) {
        char *add_argv[] = {"iptables", "-t", "nat", "-A", "POSTROUTING",
                             "-s",       NET_SUBNET_CIDR,
                             "!",        "-o", NET_BRIDGE_NAME,
                             "-j",       "MASQUERADE", NULL};
        if (net_run(add_argv) == -1)
            return -1;
    }

    return 0;
}

/* Defense in depth: same name check as cgroups.c/registry.c, not relying on the CLI alone. */
static int is_valid_name(const char *name)
{
    if (name == NULL)
        return 0;

    size_t len = strlen(name);
    if (len == 0 || len > 64)
        return 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '_' && c != '-')
            return 0;
    }

    return 1;
}

static int mkdir_if_missing(const char *path)
{
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        perror(path);
        return -1;
    }
    return 0;
}

int net_alloc_ip(const char *name, char *ip_out, size_t ip_out_size)
{
    if (!is_valid_name(name)) {
        fprintf(stderr, "net: invalid container name '%s'\n", name ? name : "(null)");
        return -1;
    }

    if (mkdir_if_missing(MOCKER_ROOT) == -1)
        return -1;
    if (mkdir_if_missing(NET_LEASES_DIR) == -1)
        return -1;

    int fd = open(NET_LEASES_FILE, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        perror(NET_LEASES_FILE);
        return -1;
    }

    /* Held across the read-modify-write so concurrent runs can't pick the same IP. */
    if (flock(fd, LOCK_EX) == -1) {
        perror(NET_LEASES_FILE);
        close(fd);
        return -1;
    }

    FILE *f = fdopen(fd, "r+");
    if (f == NULL) {
        perror(NET_LEASES_FILE);
        close(fd);
        return -1;
    }

    int used[255];
    memset(used, 0, sizeof(used));

    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char lease_name[65];
        int a, b, c, d;
        if (sscanf(line, "%64s %d.%d.%d.%d", lease_name, &a, &b, &c, &d) == 5)
            if (d >= 0 && d < 255)
                used[d] = 1;
    }

    int octet = -1;
    for (int i = 2; i <= 254; i++) {
        if (!used[i]) {
            octet = i;
            break;
        }
    }

    if (octet == -1) {
        fprintf(stderr, "net: no free IPs left in %s\n", NET_SUBNET_CIDR);
        fclose(f);
        return -1;
    }

    int n = snprintf(ip_out, ip_out_size, "10.200.0.%d", octet);
    if (n < 0 || (size_t)n >= ip_out_size) {
        fprintf(stderr, "net: ip buffer too small\n");
        fclose(f);
        return -1;
    }

    /* C requires a seek between a read and a write on an "r+" stream. */
    fseek(f, 0, SEEK_END);
    fprintf(f, "%s %s\n", name, ip_out);
    fflush(f);

    fclose(f); /* also releases the flock() */
    return 0;
}

static unsigned int djb2(const char *s)
{
    unsigned int h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++)
        h = ((h << 5) + h) + *p;
    return h;
}

void net_veth_names(const char *name, char *host_veth_out, char *peer_veth_out)
{
    unsigned int hash = djb2(name) & 0xFFFFu;
    snprintf(host_veth_out, NET_IFNAME_MAX, "v%.6s%04x", name, hash);
    snprintf(peer_veth_out, NET_IFNAME_MAX, "p%.6s%04x", name, hash);
}

int net_veth_setup(const char *host_veth, const char *peer_veth, pid_t pid)
{
    char *add_argv[] = {"ip",         "link", "add",  (char *)host_veth, "type",
                         "veth",       "peer", "name", (char *)peer_veth, NULL};
    if (net_run(add_argv) == -1)
        return -1;

    char pid_buf[16];
    snprintf(pid_buf, sizeof(pid_buf), "%d", (int)pid);
    char *netns_argv[] = {"ip", "link", "set", (char *)peer_veth, "netns", pid_buf, NULL};
    if (net_run(netns_argv) == -1)
        return -1;

    char *master_argv[] = {"ip", "link", "set", (char *)host_veth, "master", NET_BRIDGE_NAME,
                            NULL};
    if (net_run(master_argv) == -1)
        return -1;

    char *up_argv[] = {"ip", "link", "set", (char *)host_veth, "up", NULL};
    if (net_run(up_argv) == -1)
        return -1;

    return 0;
}

int net_container_configure(const char *peer_veth, const char *ip)
{
    char *rename_argv[] = {"ip", "link", "set", (char *)peer_veth, "name", "eth0", NULL};
    if (net_run(rename_argv) == -1)
        return -1;

    char ip_cidr[32];
    snprintf(ip_cidr, sizeof(ip_cidr), "%s/24", ip);
    char *addr_argv[] = {"ip", "addr", "add", ip_cidr, "dev", "eth0", NULL};
    if (net_run(addr_argv) == -1)
        return -1;

    char *eth_up_argv[] = {"ip", "link", "set", "eth0", "up", NULL};
    if (net_run(eth_up_argv) == -1)
        return -1;

    char *lo_up_argv[] = {"ip", "link", "set", "lo", "up", NULL};
    if (net_run(lo_up_argv) == -1)
        return -1;

    char *route_argv[] = {"ip", "route", "add", "default", "via", NET_BRIDGE_IP, NULL};
    if (net_run(route_argv) == -1)
        return -1;

    return 0;
}

int net_release_ip(const char *name)
{
    if (!is_valid_name(name)) {
        fprintf(stderr, "net: invalid container name '%s'\n", name ? name : "(null)");
        return -1;
    }

    int fd = open(NET_LEASES_FILE, O_RDWR);
    if (fd == -1) {
        if (errno == ENOENT)
            return 0; /* nothing leased */
        perror(NET_LEASES_FILE);
        return -1;
    }

    if (flock(fd, LOCK_EX) == -1) {
        perror(NET_LEASES_FILE);
        close(fd);
        return -1;
    }

    FILE *f = fdopen(fd, "r+");
    if (f == NULL) {
        perror(NET_LEASES_FILE);
        close(fd);
        return -1;
    }

    /* The /24 only has octets 2-254, so a fixed-size table is enough. */
    char kept_lines[255][128];
    int kept = 0;

    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char lease_name[REGISTRY_NAME_MAX];
        if (sscanf(line, "%64s", lease_name) == 1 && strcmp(lease_name, name) == 0)
            continue; /* drop this container's lease */
        if (kept < 255)
            snprintf(kept_lines[kept++], sizeof(kept_lines[0]), "%s", line);
    }

    /* Rewrite the file without the dropped line, under the same lock. */
    if (ftruncate(fileno(f), 0) == -1) {
        perror(NET_LEASES_FILE);
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);
    for (int i = 0; i < kept; i++)
        fputs(kept_lines[i], f);
    fflush(f);

    fclose(f); /* also releases the flock() */
    return 0;
}

int net_veth_delete(const char *host_veth)
{
    if (if_nametoindex(host_veth) == 0)
        return 0; /* already gone: idempotent */

    char *del_argv[] = {"ip", "link", "delete", (char *)host_veth, NULL};
    return net_run(del_argv);
}

int net_port_unmap(const char *container_ip, int host_port, int container_port)
{
    char host_port_buf[16];
    char container_port_buf[16];
    snprintf(host_port_buf, sizeof(host_port_buf), "%d", host_port);
    snprintf(container_port_buf, sizeof(container_port_buf), "%d", container_port);

    char dest[64];
    snprintf(dest, sizeof(dest), "%s:%d", container_ip, container_port);

    /* Try all three rules even if one is already gone, so a partial setup doesn't block rm. */
    int ret = 0;

    char *pre_argv[] = {"iptables", "-t",      "nat",           "-D",   "PREROUTING",
                         "-p",       "tcp",     "--dport",       host_port_buf,
                         "-j",       "DNAT",    "--to-destination", dest, NULL};
    if (net_run(pre_argv) == -1)
        ret = -1;

    char *out_argv[] = {"iptables", "-t",      "nat",           "-D",   "OUTPUT",
                         "-p",       "tcp",     "--dport",       host_port_buf,
                         "-j",       "DNAT",    "--to-destination", dest, NULL};
    if (net_run(out_argv) == -1)
        ret = -1;

    char *fwd_argv[] = {"iptables", "-D", "FORWARD", "-p", "tcp", "-d", (char *)container_ip,
                         "--dport",  container_port_buf, "-j", "ACCEPT", NULL};
    if (net_run(fwd_argv) == -1)
        ret = -1;

    return ret;
}

int net_port_map(const char *container_ip, int host_port, int container_port)
{
    char host_port_buf[16];
    char container_port_buf[16];
    snprintf(host_port_buf, sizeof(host_port_buf), "%d", host_port);
    snprintf(container_port_buf, sizeof(container_port_buf), "%d", container_port);

    char dest[64];
    snprintf(dest, sizeof(dest), "%s:%d", container_ip, container_port);

    /* PREROUTING handles outside traffic; OUTPUT handles host-originated (curl localhost:<port>). */
    char *pre_argv[] = {"iptables", "-t",      "nat",           "-A",   "PREROUTING",
                         "-p",       "tcp",     "--dport",       host_port_buf,
                         "-j",       "DNAT",    "--to-destination", dest, NULL};
    if (net_run(pre_argv) == -1)
        return -1;

    char *out_argv[] = {"iptables", "-t",      "nat",           "-A",   "OUTPUT",
                         "-p",       "tcp",     "--dport",       host_port_buf,
                         "-j",       "DNAT",    "--to-destination", dest, NULL};
    if (net_run(out_argv) == -1)
        return -1;

    char *fwd_argv[] = {"iptables", "-A", "FORWARD", "-p", "tcp", "-d", (char *)container_ip,
                         "--dport",  container_port_buf, "-j", "ACCEPT", NULL};
    if (net_run(fwd_argv) == -1)
        return -1;

    return 0;
}
