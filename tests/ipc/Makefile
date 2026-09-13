#---------------------------------------------------------------------------------
# Host side tests for the hand written CMIF IPC server in sysmodule/.
#
# These tests use libnx's own header-only CMIF encoder (switch/sf/cmif.h) to
# build requests and check that the sysmodule parses them the way libnx sends
# them. No console and no devkitA64 compiler are required, only the headers.
#
#   make -C tests/ipc         build and run
#   make -C tests/ipc clean   remove the build directory
#---------------------------------------------------------------------------------

CC         ?= cc
CFLAGS     ?= -std=c11 -O2 -g
CFLAGS     += -Wall -Wextra -Werror

ROOT       := ../..
DEVKITPRO  ?= /opt/devkitpro
SYSROOT    := $(ROOT)/sysmodule
COMMON     := $(ROOT)/common/include
INCLUDES   := -I$(SYSROOT)/include -I$(COMMON) -I$(DEVKITPRO)/libnx/include
BUILD      := build

TESTS  := $(wildcard test_*.c)
BINS   := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))
HEADERS := $(wildcard $(SYSROOT)/include/dglab/*.h) $(wildcard $(COMMON)/dglab/*.h)

.PHONY: all test clean

all: test

test: $(BINS)
	@for bin in $(BINS); do \
		echo "== $$(basename $$bin)"; \
		$$bin || exit 1; \
	done

$(BUILD)/test_%: test_%.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@

clean:
	@rm -rf $(BUILD)
