CC = gcc

CFLAGS = -Wall -Wextra -Icli/src

TARGET = mocker

SRC = cli/src/main.c \
      cli/src/process/process.c \
      cli/src/namespace/namespace.c \
      cli/src/namespace/uts.c \
      cli/src/namespace/pid.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)