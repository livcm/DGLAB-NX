#pragma once

// Sensor side of the motion mode: this is the only file that talks to libnx's
// HID six axis API (see docs/joycon-input.md). It hands the raw readings to
// dglab/nro/motion_feed.h, which does the mapping and stays platform
// independent.

#include <dglab/nro/motion_feed.h>

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DglabJoycon_Left = 0,
    DglabJoycon_Right,
} DglabJoyconSide;

/// Acquires the sensor handles and starts them. Returns false when no handle
/// could be started at all; call it when the mode is entered.
bool dglabJoyconStart(void);

/// Stops the sensors again. Safe to call when they were never started.
void dglabJoyconStop(void);

/// Whether any sensor handle is running.
bool dglabJoyconStarted(void);

/// Reads everything the given side produced since the last call (the sensor
/// runs faster than the frame loop, so this is a small batch, newest last).
/// Interpolated and disconnected readings are dropped here. Returns how many
/// samples were written, never more than `max`.
size_t dglabJoyconPoll(DglabJoyconSide side, DglabMotionSample* out, size_t max);

/// Whether a reading for this side was connected the last time we looked.
bool dglabJoyconIsConnected(DglabJoyconSide side);
