#---------------------------------------------------------------------------------
# Host side tests for the platform independent network layer.
#
#   make -C tests/net         build and run
#   make -C tests/net clean   remove the build directory
#---------------------------------------------------------------------------------

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Werror

ROOT     := ../..
SYSROOT  := $(ROOT)/sysmodule
NET_DIR  := $(SYSROOT)/source/net
PROTO_DIR := $(SYSROOT)/source/protocol
COMMON   := $(ROOT)/common/include
# hostshim supplies a stand in for <switch/types.h>, see hostshim/switch/types.h.
INCLUDES := -Ihostshim -I$(SYSROOT)/include -I$(COMMON)
BUILD    := build

TESTS  := $(wildcard test_*.c)
BINS   := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))
SOURCES := $(wildcard $(NET_DIR)/*.c) $(wildcard $(PROTO_DIR)/*.c)
HEADERS := $(wildcard $(SYSROOT)/include/dglab/net/*.h) $(wildcard $(COMMON)/dglab/*.h)

.PHONY: all test clean

all: test

test: $(BINS)
	@for bin in $(BINS); do \
		echo "== $$(basename $$bin)"; \
		$$bin || exit 1; \
	done

$(BUILD)/test_%: test_%.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(SOURCES) -o $@

clean:
	@rm -rf $(BUILD)
