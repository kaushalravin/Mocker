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

# Standalone stage-2 cgroups OOM-kill proof; not part of the mocker binary.
CGROUP_TEST_TARGET = cgroup-test

CGROUP_TEST_SRC = cli/src/cgroups/cgroups.c \
                  cli/src/cgroups/cgroups_test.c

# mockerhost control-plane server; talks to ./mocker only through its CLI.
MOCKERHOST_TARGET = mockerhost/mockerhost-server

MOCKERHOST_SRC = mockerhost/src/server.c

# Teardown helper: the only code that deletes a deploy's rootfs copy (used by teardown.sh).
RMTREE_TARGET = mockerhost/mockerhost-rmtree

RMTREE_SRC = mockerhost/src/rmtree.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

$(CGROUP_TEST_TARGET): $(CGROUP_TEST_SRC)
	$(CC) $(CFLAGS) $(CGROUP_TEST_SRC) -o $(CGROUP_TEST_TARGET)

$(MOCKERHOST_TARGET): $(MOCKERHOST_SRC)
	$(CC) $(CFLAGS) $(MOCKERHOST_SRC) -o $(MOCKERHOST_TARGET)

$(RMTREE_TARGET): $(RMTREE_SRC)
	$(CC) $(CFLAGS) $(RMTREE_SRC) -o $(RMTREE_TARGET)

# Demo programs (demo/README.md), built -static: the BusyBox rootfs has no libc.
DEMO_TARGETS = demo/leak demo/forkbomb

demo/%: demo/%.c
	$(CC) -Wall -Wextra -static $< -o $@

demos: $(DEMO_TARGETS)

BASE_ROOTFS ?= /root/test-rootfs

# Runs from mockerhost/ (it reads static/ relative to its cwd). Override the template rootfs:
#   make run-mockerhost BASE_ROOTFS=/path/to/rootfs
run-mockerhost: $(MOCKERHOST_TARGET) $(RMTREE_TARGET) $(TARGET)
	cd mockerhost && sudo MOCKERHOST_BASE_ROOTFS=$(BASE_ROOTFS) ./mockerhost-server

.PHONY: run-mockerhost demos clean

clean:
	rm -f $(TARGET) $(CGROUP_TEST_TARGET) $(MOCKERHOST_TARGET) $(RMTREE_TARGET) $(DEMO_TARGETS)