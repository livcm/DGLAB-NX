#---------------------------------------------------------------------------------
# Host side tests for the NRO's software canvas.
#
#   make -C tests/canvas         build and run
#   make -C tests/canvas clean   remove the build directory
#---------------------------------------------------------------------------------

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Werror

ROOT     := ../..
NRO      := $(ROOT)/nro
COMMON   := $(ROOT)/common/include
# hostshim supplies a stand in for <switch/types.h>, see tests/net/hostshim.
INCLUDES := -I../net/hostshim -I$(NRO)/include -I$(COMMON)
BUILD    := build

TESTS  := $(wildcard test_*.c)
BINS   := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))
SOURCES := $(wildcard $(NRO)/source/ui/*.c)
HEADERS := $(wildcard $(NRO)/include/dglab/ui/*.h) $(wildcard $(COMMON)/dglab/*.h)

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
