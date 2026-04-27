# Assume "CROSS_COMPILE" set to "aarch64-none-linux-gnu-"
CROSS_COMPILE ?=
CC ?= $(CROSS_COMPILE)gcc

# Linker and compile options
LDFLAGS ?= -pthread -lrt
CFLAGS ?= -g -Wall -Werror

# Default target
all:
	$(CC) $(CFLAGS) -c -o aesdsocket.o aesdsocket.c $(LDFLAGS)
	$(CC) $(CFLAGS) -o aesdsocket aesdsocket.o $(LDFLAGS)

# Clean target
clean:
	rm -f aesdsocket *.o