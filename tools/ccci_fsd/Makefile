CC ?= aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

all: ccci_fsd

ccci_fsd: ccci_fsd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ccci_fsd

.PHONY: all clean
