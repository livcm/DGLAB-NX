#---------------------------------------------------------------------------------
# Host side check of the sysmodule's stack budget.
#
# The sysmodule's threads have small stacks (the main thread's is
# main_thread_stack_size in sysmodule/DGLAB-NX-Core.json, 32KB) and its IPC chain
# runs libnx's IPC, newlib's printf and the SD card logging underneath. A single
# KB-scale local buffer is enough to kill the process - and a killed sysmodule
# answers no IPC at all, which reads as "the NRO froze" (docs/dglab-socket.md,
# "栈上不要放 KB 级缓冲区") - so the frames are measured on every run instead of by
# hand.
#
#   make -C tests/stack         build and check
#   make -C tests/stack clean   remove the build directory
#---------------------------------------------------------------------------------

DEVKITPRO ?= /opt/devkitpro
CC        := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc

ROOT   := ../..
SYS    := $(ROOT)/sysmodule
COMMON := $(ROOT)/common/include
BUILD  := build

# Frames at or above this are reported unless they are listed below. The number
# is deliberately small: everything that needs more belongs in .bss, which costs
# nothing here because every send already runs under the transport lock.
LIMIT := 1024

# Frames allowed to be large, and why:
#   netClientThreadMain, wsConnRecv - one receive buffer per connection, and they
#       have to stay on their own thread's stack because two clients can be
#       online at once (sysmodule/AGENTS.md, "线程与栈");
#   pocPollScanResults, pocRunBtdrvScanProbe, pocDrainGattOperations - the BLE
#       PoC's scan result structs. The PoC is diagnostic only and the BLE route
#       is shelved (docs/ble-poc.md), so they are not worth a refactor; they are
#       listed so a *new* large frame still fails the check.
ALLOW := netClientThreadMain wsConnRecv pocPollScanResults pocRunBtdrvScanProbe pocDrainGattOperations

SOURCES := $(wildcard $(SYS)/source/*.c) $(wildcard $(SYS)/source/*/*.c)
OBJECTS := $(patsubst $(SYS)/source/%.c,$(BUILD)/%.o,$(SOURCES))
USAGE   := $(OBJECTS:.o=.su)
HEADERS := $(wildcard $(SYS)/include/dglab/*.h) $(wildcard $(SYS)/include/dglab/*/*.h) \
	$(wildcard $(COMMON)/dglab/*.h)

.PHONY: all test clean

all: test

test: $(USAGE)
	@awk -v limit=$(LIMIT) -v allow="$(ALLOW)" -f check_frames.awk $(USAGE)

$(BUILD)/%.o: $(SYS)/source/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -c -fstack-usage -I$(SYS)/include -I$(COMMON) \
		-I$(DEVKITPRO)/libnx/include $< -o $@

# gcc writes the .su next to the object as a side effect of the compile above, so
# this rule only asserts that it is really there.
$(BUILD)/%.su: $(BUILD)/%.o
	@test -f $@ || { echo "gcc wrote no stack usage for $<"; exit 1; }

clean:
	@rm -rf $(BUILD)
