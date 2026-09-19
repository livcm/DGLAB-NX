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
# The NRO diagnostics these tests compile are cut on purpose: the log line
# langfiles.c writes for a bad language file is one 40 character panel line,
# and lang_fixture.h names a path that can be longer than the buffer it lands
# in. GCC's -Wformat-truncation cannot tell those intended cuts from a bug and
# -Werror turns it into a build failure, which is what the CI container's GCC
# did while the clang this is developed with has no such warning at all (it
# ignores the option), so the flag only ever does something on GCC.
CFLAGS   += -Wno-format-truncation

ROOT     := ../..
NRO      := $(ROOT)/nro
# The tests read the real files, so a translation that lost a key fails here.
LANG_DIR := $(abspath $(ROOT)/lang)

# settings.c owns the NRO's app.cfg, theme.c the two palettes and the theme
# preference, so both are checked here next to the language preference they sit
# beside on the SD card.
SOURCES  := $(NRO)/source/ui/strings.c $(NRO)/source/ui/language.c \
	$(NRO)/source/ui/settings.c $(NRO)/source/ui/theme.c \
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
