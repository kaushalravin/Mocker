#define _GNU_SOURCE

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cgroups/cgroups.h"
#include "network/net_setup.h"
#include "process/process.h"
#include "registry/registry.h"

/* mocker CLI: run / stop / rm / ps / inspect.
 * Usage: mocker run --name <name> [--mem-mb <n>] [--pids <n>] [--cpu-percent <n>]
 *                   [--port <hostport>:<containerport>] <rootfs-path> -- <cmd> [args...] */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s run --name <name> [--mem-mb <n>] [--pids <n>] "
            "[--cpu-percent <n>] [--port <hostport>:<containerport>] "
            "<rootfs-path> -- <cmd> [args...]\n"
            "       %s stop <name>\n"
            "       %s rm <name>\n"
            "       %s ps\n"
            "       %s inspect <name>\n",
            prog, prog, prog, prog, prog);
}

/* Liveness from /proc/<pid>, never the stored status string (it can go stale). */
static int process_alive(pid_t pid)
{
    char proc_path[32];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", (int)pid);
    return access(proc_path, F_OK) == 0;
}

/* Parses "hostport:containerport" (both 1-65535) or exits with an error. */
static void parse_port_map(const char *value, int *host_port_out, int *container_port_out)
{
    int host_port, container_port;
    if (sscanf(value, "%d:%d", &host_port, &container_port) != 2 ||
        host_port < 1 || host_port > 65535 || container_port < 1 || container_port > 65535) {
        fprintf(stderr,
                "mocker: --port expects <hostport>:<containerport> (1-65535), got '%s'\n",
                value);
        exit(EXIT_FAILURE);
    }
    *host_port_out = host_port;
    *container_port_out = container_port;
}

/* Parses a numeric CLI flag or exits with a usage error. */
static long parse_positive_long(const char *flag, const char *value)
{
    char *end;
    long n = strtol(value, &end, 10);
    if (*value == '\0' || *end != '\0' || n <= 0) {
        fprintf(stderr, "mocker: %s expects a positive integer, got '%s'\n", flag, value);
        exit(EXIT_FAILURE);
    }
    return n;
}

static int cmd_run(int argc, char *argv[])
{
    const char *name = NULL;
    long mem_mb = -1;
    long pids_max = -1;
    long cpu_percent = -1;
    int host_port = -1;
    int container_port = -1;

    int i = 2;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0 && strcmp(argv[i], "--") != 0; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--mem-mb") == 0 && i + 1 < argc) {
            mem_mb = parse_positive_long("--mem-mb", argv[++i]);
        } else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc) {
            pids_max = parse_positive_long("--pids", argv[++i]);
        } else if (strcmp(argv[i], "--cpu-percent") == 0 && i + 1 < argc) {
            cpu_percent = parse_positive_long("--cpu-percent", argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            parse_port_map(argv[++i], &host_port, &container_port);
        } else {
            fprintf(stderr, "mocker: unrecognized or malformed flag '%s'\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (name == NULL || i >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!registry_valid_name(name)) {
        fprintf(stderr,
                "mocker: invalid --name '%s' - must match ^[a-zA-Z0-9_-]{1,64}$\n", name);
        return EXIT_FAILURE;
    }

    const char *rootfs_arg = argv[i++];

    if (i >= argc || strcmp(argv[i], "--") != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    i++; /* skip "--" */

    if (i >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    char **cmd_argv = &argv[i]; /* argv[argc] is NULL, so already terminated */

    char rootfs_path[PATH_MAX];
    if (realpath(rootfs_arg, rootfs_path) == NULL) {
        perror(rootfs_arg);
        return EXIT_FAILURE;
    }

    if (!cgroups_v2_available()) {
        fprintf(stderr,
                "mocker: /sys/fs/cgroup is not cgroups v2 - refusing to run "
                "(check `mount | grep cgroup2`)\n");
        return EXIT_FAILURE;
    }

    if (cgroup_create(name) == -1)
        return EXIT_FAILURE;

    cgroup_limits_t limits = {
        .mem_max_bytes = mem_mb > 0 ? mem_mb * 1024 * 1024 : -1,
        .pids_max = pids_max,
        .cpu_max_percent = (int)cpu_percent,
    };

    if (cgroup_apply_limits(name, &limits) == -1) {
        cgroup_remove(name);
        return EXIT_FAILURE;
    }

    char cgroup_path[PATH_MAX];
    if (cgroup_dir_path(name, cgroup_path, sizeof(cgroup_path)) == -1) {
        cgroup_remove(name);
        return EXIT_FAILURE;
    }

    /* Bridge and NAT are shared host-wide state; ensure them once up front. */
    if (net_bridge_ensure() == -1) {
        cgroup_remove(name);
        return EXIT_FAILURE;
    }

    char container_ip[32];
    if (net_alloc_ip(name, container_ip, sizeof(container_ip)) == -1) {
        cgroup_remove(name);
        return EXIT_FAILURE;
    }

    char host_veth[NET_IFNAME_MAX];
    char peer_veth[NET_IFNAME_MAX];
    net_veth_names(name, host_veth, peer_veth);

    printf("MOCKER: starting container '%s' (rootfs=%s, ip=%s)...\n", name, rootfs_path,
           container_ip);
    fflush(stdout);

    int net_ready_fd;
    pid_t pid = create_process(rootfs_path, name, cmd_argv, peer_veth, container_ip,
                                &net_ready_fd);

    printf("MOCKER: container '%s' host PID = %d\n", name, pid);
    fflush(stdout);

    /* Attach right after clone(); on failure kill the container rather than leave it unlimited. */
    if (cgroup_attach(name, pid) == -1) {
        fprintf(stderr, "mocker: failed to attach PID %d to cgroup, killing container\n", pid);
        close(net_ready_fd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        cgroup_remove(name);
        return EXIT_FAILURE;
    }

    /* Host-side veth wiring; PID 1 is still blocked on net_ready_fd, so there is no race.
     * Kill the container on failure. */
    if (net_veth_setup(host_veth, peer_veth, pid) == -1) {
        fprintf(stderr, "mocker: failed to wire networking for '%s', killing container\n", name);
        close(net_ready_fd);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        cgroup_remove(name);
        return EXIT_FAILURE;
    }

    /* Release PID 1 to configure eth0 and continue to pivot_root()/execvp(). */
    if (write(net_ready_fd, "x", 1) != 1)
        perror("net_ready_fd write");
    close(net_ready_fd);

    if (host_port != -1) {
        /* Non-fatal: port mapping is a convenience, not core isolation. */
        if (net_port_map(container_ip, host_port, container_port) == -1)
            fprintf(stderr, "mocker: warning: port mapping %d:%d failed\n", host_port,
                    container_port);
    }

    registry_entry_t entry = {
        .name = name,
        .pid = pid,
        .rootfs_path = rootfs_path,
        .cgroup_path = cgroup_path,
        .ip = container_ip,
        .status = "running",
        .host_port = host_port,
        .container_port = container_port,
    };
    registry_write(&entry);

    wait_for_process(pid);

    entry.status = "exited";
    registry_write(&entry);

    /* State is left for `mocker rm`, so ps/inspect can still show the exited container. */
    printf("MOCKER: container '%s' done (state left in place - run `mocker "
           "rm %s` to clean up).\n",
           name, name);

    return EXIT_SUCCESS;
}

static registry_entry_t entry_from_record(const registry_record_t *rec)
{
    registry_entry_t entry = {
        .name = rec->name,
        .pid = rec->pid,
        .rootfs_path = rec->rootfs_path,
        .cgroup_path = rec->cgroup_path,
        .ip = rec->ip,
        .status = rec->status,
        .host_port = rec->host_port,
        .container_port = rec->container_port,
    };
    return entry;
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char *name = argv[2];

    registry_record_t rec;
    if (registry_read(name, &rec) == -1) {
        fprintf(stderr, "mocker: no such container '%s'\n", name);
        return EXIT_FAILURE;
    }

    if (!process_alive(rec.pid)) {
        printf("mocker: '%s' is not running\n", name);
        return EXIT_SUCCESS;
    }

    /* SIGTERM, then SIGKILL after a timeout: PID 1 of a PID namespace ignores signals it has
     * no handler for, so SIGTERM alone often hangs (docs/architecture.md section 8). */
    printf("MOCKER: sending SIGTERM to '%s' (pid %d)...\n", name, (int)rec.pid);
    fflush(stdout);
    kill(rec.pid, SIGTERM);

    const int timeout_ms = 5000;
    const int poll_ms = 200;
    int waited_ms = 0;
    while (waited_ms < timeout_ms && process_alive(rec.pid)) {
        usleep(poll_ms * 1000);
        waited_ms += poll_ms;
    }

    if (process_alive(rec.pid)) {
        printf("MOCKER: '%s' did not exit after SIGTERM, sending SIGKILL\n", name);
        fflush(stdout);
        kill(rec.pid, SIGKILL);
        waited_ms = 0;
        while (waited_ms < timeout_ms && process_alive(rec.pid)) {
            usleep(poll_ms * 1000);
            waited_ms += poll_ms;
        }
    }

    if (process_alive(rec.pid)) {
        fprintf(stderr, "mocker: '%s' (pid %d) did not stop\n", name, (int)rec.pid);
        return EXIT_FAILURE;
    }

    registry_entry_t entry = entry_from_record(&rec);
    entry.status = "exited";
    registry_write(&entry);

    printf("MOCKER: '%s' stopped\n", name);
    return EXIT_SUCCESS;
}

static int cmd_rm(int argc, char *argv[])
{
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char *name = argv[2];

    registry_record_t rec;
    if (registry_read(name, &rec) == -1) {
        fprintf(stderr, "mocker: no such container '%s'\n", name);
        return EXIT_FAILURE;
    }

    if (process_alive(rec.pid)) {
        fprintf(stderr, "mocker: '%s' is still running (pid %d) - `mocker stop` it first\n",
                name, (int)rec.pid);
        return EXIT_FAILURE;
    }

    /* Never touches the user-supplied rootfs; only removes what Mocker created. Every step is
     * attempted even if an earlier one warns, so a partly set-up container can still be removed. */
    int had_warning = 0;

    if (cgroup_remove(name) == -1) {
        fprintf(stderr, "mocker: warning: failed to remove cgroup for '%s'\n", name);
        had_warning = 1;
    }

    char host_veth[NET_IFNAME_MAX];
    char peer_veth[NET_IFNAME_MAX];
    net_veth_names(name, host_veth, peer_veth);
    if (net_veth_delete(host_veth) == -1) {
        fprintf(stderr, "mocker: warning: failed to delete veth '%s' for '%s'\n", host_veth,
                name);
        had_warning = 1;
    }

    if (rec.host_port != -1) {
        if (net_port_unmap(rec.ip, rec.host_port, rec.container_port) == -1) {
            fprintf(stderr, "mocker: warning: failed to remove port mapping for '%s'\n", name);
            had_warning = 1;
        }
    }

    if (net_release_ip(name) == -1) {
        fprintf(stderr, "mocker: warning: failed to release IP lease for '%s'\n", name);
        had_warning = 1;
    }

    if (registry_remove(name) == -1) {
        fprintf(stderr, "mocker: failed to remove registry entry for '%s'\n", name);
        return EXIT_FAILURE;
    }

    if (had_warning)
        printf("MOCKER: removed '%s' (with warnings above - some state may need manual "
               "cleanup)\n",
               name);
    else
        printf("MOCKER: removed '%s'\n", name);

    return EXIT_SUCCESS;
}

static int cmd_ps(int argc, char *argv[])
{
    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    char names[256][REGISTRY_NAME_MAX];
    int count = registry_list(names, 256);
    if (count == -1)
        return EXIT_FAILURE;

    printf("%-20s %-8s %-16s %s\n", "NAME", "PID", "IP", "STATUS");
    for (int i = 0; i < count; i++) {
        registry_record_t rec;
        if (registry_read(names[i], &rec) == -1)
            continue;
        /* Kernel truth, not the stored status string. */
        const char *status = process_alive(rec.pid) ? "running" : "exited";
        printf("%-20s %-8d %-16s %s\n", rec.name, (int)rec.pid, rec.ip, status);
    }

    return EXIT_SUCCESS;
}

static void print_ns(const char *label, pid_t pid, const char *ns)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)pid, ns);

    char link[256];
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n == -1) {
        printf("  %s: (unavailable)\n", label);
        return;
    }
    link[n] = '\0';
    printf("  %s: %s\n", label, link);
}

static int cmd_inspect(int argc, char *argv[])
{
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char *name = argv[2];

    registry_record_t rec;
    if (registry_read(name, &rec) == -1) {
        fprintf(stderr, "mocker: no such container '%s'\n", name);
        return EXIT_FAILURE;
    }

    int alive = process_alive(rec.pid);

    printf("name: %s\n", rec.name);
    printf("pid: %d\n", (int)rec.pid);
    printf("status: %s (kernel-verified)\n", alive ? "running" : "exited");
    printf("rootfs: %s\n", rec.rootfs_path);
    printf("cgroup: %s\n", rec.cgroup_path);
    printf("ip: %s\n", rec.ip);
    if (rec.host_port != -1)
        printf("port: %d:%d\n", rec.host_port, rec.container_port);

    if (alive) {
        /* Live kernel state, not state.json. */
        printf("namespaces:\n");
        print_ns("pid", rec.pid, "pid");
        print_ns("mnt", rec.pid, "mnt");
        print_ns("uts", rec.pid, "uts");
        print_ns("net", rec.pid, "net");

        long mem_current = cgroup_get_memory_current(name);
        long pids_current = cgroup_get_pids_current(name);
        printf("cgroup usage:\n");
        if (mem_current >= 0)
            printf("  memory.current: %ld bytes\n", mem_current);
        if (pids_current >= 0)
            printf("  pids.current: %ld\n", pids_current);
    }

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);
    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);
    if (strcmp(argv[1], "rm") == 0)
        return cmd_rm(argc, argv);
    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps(argc, argv);
    if (strcmp(argv[1], "inspect") == 0)
        return cmd_inspect(argc, argv);

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
