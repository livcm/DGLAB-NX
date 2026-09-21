#pragma once

// Applet-side BLE connect probe.
//
// Diagnostic only: the real transport has to live in the sysmodule
// (AGENTS.md §3), but the sysmodule's own connect attempt never gets through
// (docs/ble-re.md). This runs the same attempt from the NRO, i.e. from an
// applet, where libnx's btdev wrappers fill in a real AppletResourceUserId -
// the path Nintendo actually supports. Nothing is written to the device.
//
// The caller owns the console (the BLE PoC view prints to it).

#include <switch/types.h>

#include <switch/services/btdrv_types.h>

/// Run one connect attempt against addr and report what the stack does.
void dglabAppletBleProbeRun(const BtdrvAddress* addr, const char* address_path);
