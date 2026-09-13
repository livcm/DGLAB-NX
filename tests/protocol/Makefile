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

TARGET  := $(BUILD)/test_coyote_v3
SOURCES := test_coyote_v3.c $(PROTOCOL_DIR)/coyote_v3.c
HEADERS := $(SYSROOT)/include/dglab/protocol/coyote_v3.h

.PHONY: all test clean

all: test

test: $(TARGET)
	@$(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(SOURCES) -o $@

clean:
	@rm -rf $(BUILD)
