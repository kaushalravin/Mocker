CC = gcc

CFLAGS = -Wall -Wextra -Isrc

TARGET = minidocker

SRC = src/main.c \
      src/process/process.c \
      src/namespace/namespace.c \
      src/namespace/uts.c \
	  src/namespace/pid.c 

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)