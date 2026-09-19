#---------------------------------------------------------------------------------
# Host side tests for the touch mode's mapping.
#
#   make -C tests/touch         build and run
#   make -C tests/touch clean   remove the build directory
#---------------------------------------------------------------------------------

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Werror
LDLIBS   ?= -lm

ROOT     := ../..
NRO      := $(ROOT)/nro
COMMON   := $(ROOT)/common/include
# hostshim supplies a stand in for <switch/types.h>, see tests/net/hostshim.
INCLUDES := -I../net/hostshim -I$(NRO)/include -I$(COMMON)
BUILD    := build

TESTS    := $(wildcard test_*.c)
BINS     := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))
# The touch mode drives the motion mode's slot producer with the parameters the
# same file holds, so both are compiled in: the tests pin down the sharing, not
# only the mapping.
SOURCES  := $(NRO)/source/touch/touch_feed.c $(NRO)/source/motion/motion_feed.c \
	$(NRO)/source/motion/motion_settings.c
HEADERS  := $(wildcard $(NRO)/include/dglab/nro/*.h) $(wildcard $(COMMON)/dglab/*.h)

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
