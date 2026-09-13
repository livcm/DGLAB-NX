#---------------------------------------------------------------------------------
# Host side tests for the platform independent protocol layer.
#
# These tests build with the system compiler, not devkitA64, because the
# protocol code deliberately contains no libnx or HOS dependency.
#
#   make -C tests/protocol         build and run
#   make -C tests/protocol clean   remove the build directory
#
# sysmodule/Makefile also compiles source/protocol for the Switch build, so the
# same sources are checked by both toolchains.
#---------------------------------------------------------------------------------

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
CFLAGS  += -Wall -Wextra -Werror

ROOT         := ../..
SYSROOT      := $(ROOT)/sysmodule
PROTOCOL_DIR := $(SYSROOT)/source/protocol
INCLUDES     := -I$(SYSROOT)/include
BUILD        := build

TESTS            := $(wildcard test_*.c)
BINS             := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))
PROTOCOL_SOURCES := $(wildcard $(PROTOCOL_DIR)/*.c)
HEADERS          := $(wildcard $(SYSROOT)/include/dglab/protocol/*.h) test_support.h

.PHONY: all test clean

all: test

test: $(BINS)
	@for bin in $(BINS); do \
		echo "== $$(basename $$bin)"; \
		$$bin || exit 1; \
	done

$(BUILD)/test_%: test_%.c $(PROTOCOL_SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(PROTOCOL_SOURCES) -o $@

clean:
	@rm -rf $(BUILD)
