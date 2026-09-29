CC = gcc

CFLAGS = -Wall -Wextra -Icli/src

TARGET = mocker

SRC = cli/src/main.c \
      cli/src/process/process.c \
      cli/src/namespace/namespace.c \
      cli/src/namespace/uts.c \
      cli/src/mount/mount_setup.c

# Stage-2 standalone cgroups test - not linked into the mocker binary yet,
# see docs/architecture.md "Combine" stage.
CGROUP_TEST_TARGET = cgroup-test

CGROUP_TEST_SRC = cli/src/cgroups/cgroups.c \
                  cli/src/cgroups/cgroups_test.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

$(CGROUP_TEST_TARGET): $(CGROUP_TEST_SRC)
	$(CC) $(CFLAGS) $(CGROUP_TEST_SRC) -o $(CGROUP_TEST_TARGET)

clean:
	rm -f $(TARGET) $(CGROUP_TEST_TARGET)