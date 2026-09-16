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
///
/// This always takes a fresh set, and **entering the mode again is the way to get
/// one**: handles describe the assignment the console had at that moment, so a
/// Joy-Con that was turned off or plugged back in during a session leaves its row
/// on "not connected" until the mode is entered again. What this deliberately
/// does *not* do is re-acquire mid-session (see joycon.c): taking a set for one
/// side while the other controller was the only one attached is how a side ended
/// up reading the other Joy-Con.
bool dglabJoyconStart(void);

/// Stops the sensors again. Safe to call when they were never started.
void dglabJoyconStop(void);

/// Takes a fresh set of handles (both sides, from one call) without touching the
/// connection state, and counts it in the log's `rescans`. This is what the
/// motion page's Y button does: a Joy-Con that was turned off or unplugged keeps
/// its row on "not connected" until the handles are taken again, and this does
/// that in place.
void dglabJoyconRescan(void);

/// Whether any sensor handle is running.
bool dglabJoyconStarted(void);

/// Reads everything the given side produced since the last call (the sensor
/// runs faster than the frame loop, so this is a small batch, newest last).
/// Interpolated and disconnected readings are dropped here. Returns how many
/// samples were written, never more than `max`.
size_t dglabJoyconPoll(DglabJoyconSide side, DglabMotionSample* out, size_t max);

/// Whether a reading for this side was connected the last time we looked.
bool dglabJoyconIsConnected(DglabJoyconSide side);

/// Writes one line describing what this side's handles are and what the last
/// poll got out of them, e.g. "left: handles 1, quiet 0, rescans 0, #0 states 16,
/// samples 16, connected 1". `quiet` counts the polls in a row without any
/// readings, which is what turns the row to "not connected" once it reaches
/// DGLAB_MOTION_SENSOR_QUIET_POLLS; `rescans` counts the fresh sets the mode took
/// because both sides had gone quiet.
/// Written into the NRO's log by main.c when the mode starts: which handles a
/// console really hands over (and which of them answer) is a hardware fact, and
/// this is what makes the next hardware run conclusive. A handle that was not
/// polled says so - "no readings" and "not asked" are different answers.
/// Returns the length written, 0 when `out` is unusable or no sensor was started.
size_t dglabJoyconDescribe(DglabJoyconSide side, char* out, size_t size);
