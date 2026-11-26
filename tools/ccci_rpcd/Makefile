CC ?= aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

all: ccci_rpcd

ccci_rpcd: ccci_rpcd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ccci_rpcd

.PHONY: all clean
