CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS := -O2 -Wall -Wextra -Werror -std=c11 -static

all: ccci_rpcd

ccci_rpcd: ccci_rpcd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ccci_rpcd

.PHONY: all clean
