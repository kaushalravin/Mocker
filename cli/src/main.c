#define _GNU_SOURCE

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "cgroups/cgroups.h"
#include "process/process.h"
#include "registry/registry.h"

/*
 * Stage 4 ("Combine", see CLAUDE.md section 6 / docs/architecture.md): the
 * real `mocker run` CLI. Namespaces + pivot_root (stage 3) and cgroups
 * (stage 2) are wired together here, in the order docs/architecture.md
 * section 3 specifies - cgroup created and limited *before* the container
 * is spawned, PID attached to it *immediately* after clone() returns, and
 * only then a registry entry recorded.
 *
 * Usage: mocker run --name <name> [--mem-mb <n>] [--pids <n>]
 *                    [--cpu-percent <n>] <rootfs-path> -- <cmd> [args...]
 */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s run --name <name> [--mem-mb <n>] [--pids <n>] "
            "[--cpu-percent <n>] <rootfs-path> -- <cmd> [args...]\n",
            prog);
}

/* Parses one non-negative CLI integer, or exits with a usage-style error -
 * every numeric flag here goes through this so bad input fails the same way
 * regardless of which flag it came from. */
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

    printf("MOCKER: starting container '%s' (rootfs=%s)...\n", name, rootfs_path);
    fflush(stdout);

    pid_t pid = create_process(rootfs_path, name, cmd_argv);

    printf("MOCKER: container '%s' host PID = %d\n", name, pid);
    fflush(stdout);

    /* Attach before the container gets far into its own setup - see
     * docs/architecture.md section 3 point 4. A failure here means the
     * container must not be left running unconstrained, so it's killed
     * rather than allowed to continue outside the cgroup. */
    if (cgroup_attach(name, pid) == -1) {
        fprintf(stderr, "mocker: failed to attach PID %d to cgroup, killing container\n", pid);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        cgroup_remove(name);
        return EXIT_FAILURE;
    }

    registry_entry_t entry = {
        .name = name,
        .pid = pid,
        .rootfs_path = rootfs_path,
        .cgroup_path = cgroup_path,
        .status = "running",
    };
    registry_write(&entry);

    wait_for_process(pid);

    entry.status = "exited";
    registry_write(&entry);

    /* Cleanup (cgroup rmdir, registry entry, rootfs) is `mocker rm`'s job -
     * deliberately not implemented yet, see docs/architecture.md section 5
     * and CLAUDE.md section 9. Both are left in place here on purpose. */
    printf("MOCKER: container '%s' done (cgroup and registry entry left in "
           "place - cleanup is `mocker rm`, not yet implemented).\n",
           name);

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    if (argc < 2 || strcmp(argv[1], "run") != 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return cmd_run(argc, argv);
}
