# Assume "CROSS_COMPILE" set to "aarch64-none-linux-gnu-"
CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

# Default target
all:
	$(CC) -g -Wall -c -o writer.o writer.c
	$(CC) -g -Wall -o writer writer.o

# Clean target
clean:
	rm -f writer *.o