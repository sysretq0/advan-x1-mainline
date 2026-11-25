# SPDX-License-Identifier: GPL-2.0-only
# Build ADVAN X1 static C /init binary (aarch64) and host unit tests.

CROSS  ?= aarch64-linux-gnu-
ifeq ($(origin CC),default)
CC     := $(CROSS)gcc
endif
STRIP  ?= $(CROSS)strip
HOSTCC ?= cc
OUT    ?= .

CFLAGS  := -std=gnu11 -Os -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS := -static -Wl,--gc-sections

.PHONY: all test clean

all: $(OUT)/init

$(OUT)/init: init.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(STRIP) --strip-all $@

test: $(OUT)/test_init
	$(OUT)/test_init

$(OUT)/test_init: test_init.c init.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function -o $@ test_init.c

clean:
	rm -f $(OUT)/init $(OUT)/test_init
