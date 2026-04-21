# Assume "CROSS_COMPILE" set to "aarch64-none-linux-gnu-"
CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

# Default target
all:
	$(CC) -g -Wall -c -o aesdsocket.o aesdsocket.c
	$(CC) -g -Wall -o aesdsocket aesdsocket.o

# Clean target
clean:
	rm -f aesdsocket *.o