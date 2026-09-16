#---------------------------------------------------------------------------------
# Host side tests for the NRO's software canvas.
#
#   make -C tests/canvas         build and run
#   make -C tests/canvas clean   remove the build directory
#---------------------------------------------------------------------------------

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Werror
# The advanced screen reads the motion settings, which use libm.
LDLIBS   ?= -lm

ROOT     := ../..
NRO      := $(ROOT)/nro
COMMON   := $(ROOT)/common/include
# hostshim supplies a stand in for <switch/types.h>, see tests/net/hostshim.
# The screens draw the text of the files the NRO ships, so the tests read the
# real lang/*.json (see tests/lang/lang_fixture.h).
INCLUDES := -I../net/hostshim -I$(NRO)/include -I$(COMMON) -I$(ROOT)/third_party \
	-DDGLAB_TEST_LANG_DIR='"$(abspath $(ROOT)/lang)"'
BUILD    := build

TESTS  := $(wildcard test_*.c)
BINS   := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))
# joycon.c is left out on purpose: it is the one motion file that needs libnx.
SOURCES := $(wildcard $(NRO)/source/ui/*.c) $(NRO)/source/motion/motion_feed.c \
	$(NRO)/source/motion/motion_settings.c $(NRO)/source/util/json.c
HEADERS := $(wildcard $(NRO)/include/dglab/ui/*.h) $(wildcard $(NRO)/include/dglab/nro/*.h) \
	$(wildcard $(COMMON)/dglab/*.h)

.PHONY: all test clean

all: test

test: $(BINS)
	@for bin in $(BINS); do \
		echo "== $$(basename $$bin)"; \
		$$bin || exit 1; \
	done

$(BUILD)/test_%: test_%.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(SOURCES) -o $@ $(LDLIBS)

clean:
	@rm -rf $(BUILD)
