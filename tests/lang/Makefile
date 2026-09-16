#---------------------------------------------------------------------------------
# Host side tests for the language files: the JSON reader, the string loader,
# and the lang/*.json the NRO ships.
#
#   make -C tests/lang         build and run
#   make -C tests/lang clean   remove the build directory
#---------------------------------------------------------------------------------

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Werror

ROOT     := ../..
NRO      := $(ROOT)/nro
# The tests read the real files, so a translation that lost a key fails here.
LANG_DIR := $(abspath $(ROOT)/lang)

SOURCES  := $(NRO)/source/ui/strings.c $(NRO)/source/ui/language.c \
	$(NRO)/source/util/json.c $(NRO)/source/platform/langfiles.c
# langfiles.c is the part that reads the SD card on a console, but it only uses
# stdio, so a directory on the host drives the same code.
INCLUDES := -I$(NRO)/include -DDGLAB_TEST_LANG_DIR='"$(LANG_DIR)"'
BUILD    := build

TESTS  := $(wildcard test_*.c)
BINS   := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))

.PHONY: all test clean

all: test

test: $(BINS)
	@for bin in $(BINS); do \
		echo "== $$(basename $$bin)"; \
		$$bin || exit 1; \
	done

$(BUILD)/test_%: test_%.c $(SOURCES) lang_fixture.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(SOURCES) -o $@

clean:
	@rm -rf $(BUILD)
