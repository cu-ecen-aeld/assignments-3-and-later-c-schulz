# Assume "CROSS_COMPILE" set to "aarch64-none-linux-gnu-"

# Default target
all:
	$(CROSS_COMPILE)gcc -g -Wall -c -o writer.o writer.c
	$(CROSS_COMPILE)gcc -g -Wall -o writer writer.o

# Clean target
clean:
	rm -f writer *.o