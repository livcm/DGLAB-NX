#---------------------------------------------------------------------------------
# Host side tests for the NRO's QR encoder.
#
# The golden matrices under golden/ were produced by macOS CoreImage
# (CIQRCodeGenerator) with tools/gen_reference.m, so the encoder is checked
# against an independent implementation rather than against itself.
#
#   make -C tests/qr         build and run
#   make -C tests/qr clean   remove the build directory
#---------------------------------------------------------------------------------

CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -g
CFLAGS   += -Wall -Wextra -Werror

ROOT     := ../..
NRO      := $(ROOT)/nro
INCLUDES := -I$(NRO)/include
BUILD    := build

TESTS  := $(wildcard test_*.c)
BINS   := $(patsubst test_%.c,$(BUILD)/test_%,$(TESTS))
# Only the encoder: the canvas and the screen have tests of their own and would
# drag the IPC headers in here.
SOURCES := $(NRO)/source/ui/qr.c
HEADERS := $(NRO)/include/dglab/ui/qr.h
GOLDEN  := $(wildcard golden/*.txt)

.PHONY: all test clean

all: test

test: $(BINS) $(GOLDEN)
	@for bin in $(BINS); do \
		echo "== $$(basename $$bin)"; \
		$$bin || exit 1; \
	done

$(BUILD)/test_%: test_%.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(SOURCES) -o $@

clean:
	@rm -rf $(BUILD)
