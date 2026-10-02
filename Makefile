CC = gcc

CFLAGS = -Wall -Wextra -Icli/src

TARGET = mocker

SRC = cli/src/main.c \
      cli/src/process/process.c \
      cli/src/namespace/namespace.c \
      cli/src/namespace/uts.c \
      cli/src/mount/mount_setup.c \
      cli/src/cgroups/cgroups.c \
      cli/src/registry/registry.c \
      cli/src/network/net_setup.c

# Standalone stage-2 cgroups OOM-kill proof - separate from the mocker
# binary; it exercises cgroups.c on its own, not through `mocker run`.
CGROUP_TEST_TARGET = cgroup-test

CGROUP_TEST_SRC = cli/src/cgroups/cgroups.c \
                  cli/src/cgroups/cgroups_test.c

# mockerhost's control-plane HTTP server - independent binary, independent of
# cli/src/*.c; it talks to ./mocker only via its CLI (mockerhost/README.md).
MOCKERHOST_TARGET = mockerhost/mockerhost-server

MOCKERHOST_SRC = mockerhost/src/server.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

$(CGROUP_TEST_TARGET): $(CGROUP_TEST_SRC)
	$(CC) $(CFLAGS) $(CGROUP_TEST_SRC) -o $(CGROUP_TEST_TARGET)

$(MOCKERHOST_TARGET): $(MOCKERHOST_SRC)
	$(CC) $(CFLAGS) $(MOCKERHOST_SRC) -o $(MOCKERHOST_TARGET)

# Run the control server from inside mockerhost/ (it reads static/ and
# clone_and_build.sh relative to its cwd). Override the template rootfs with
#   make run-mockerhost BASE_ROOTFS=/path/to/rootfs
BASE_ROOTFS ?= /root/test-rootfs

run-mockerhost: $(MOCKERHOST_TARGET) $(TARGET)
	cd mockerhost && sudo MOCKERHOST_BASE_ROOTFS=$(BASE_ROOTFS) ./mockerhost-server

.PHONY: run-mockerhost clean

clean:
	rm -f $(TARGET) $(CGROUP_TEST_TARGET) $(MOCKERHOST_TARGET)