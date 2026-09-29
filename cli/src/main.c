#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "process/process.h"

/*
 * Stage-3 test harness, not the final `mocker run` CLI (that's the CLI MVP
 * work in CLAUDE.md section 6). Just enough argv handling to launch one
 * namespaced + pivot_root'd container, so PID/mount/UTS isolation can be
 * proven independently before stage 4 wires in cgroups and a real registry.
 *
 * Usage: mocker <rootfs-path> -- <cmd> [args...]
 */
int main(int argc, char *argv[])
{
    if (argc < 4 || strcmp(argv[2], "--") != 0) {
        fprintf(stderr, "usage: %s <rootfs-path> -- <cmd> [args...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *rootfs_path = argv[1];
    char **cmd_argv = &argv[3]; /* argv[argc] is NULL, so this is already terminated */

    printf("MOCKER: starting container (rootfs=%s)...\n", rootfs_path);
    fflush(stdout);

    pid_t pid = create_process(rootfs_path, cmd_argv);

    printf("MOCKER: container host PID = %d\n", pid);
    fflush(stdout);

    wait_for_process(pid);

    printf("MOCKER: done.\n");
    return 0;
}
