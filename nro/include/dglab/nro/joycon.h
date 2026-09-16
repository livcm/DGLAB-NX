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
/// This always takes a fresh set: handles acquired during an earlier visit can
/// describe an assignment the system has since changed (a Joy-Con that was
/// plugged in, turned off or re-synced), and polling one of those is what leaves
/// a side stuck on "not connected" with its channel silent.
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

/// Writes one line describing what this side's handles are and what the last
/// poll got out of them, e.g. "left: handles 2, quiet 0, rescans 0, #0 states 16,
/// samples 16, connected 1, #1 not polled". `quiet` counts the polls in a row
/// without any readings, which is what turns the row to "not connected" once it
/// reaches DGLAB_MOTION_SENSOR_QUIET_POLLS; `rescans` counts how often the side
/// gave up on its handles and took a fresh set.
/// Written into the NRO's log by main.c when the mode starts: which handles a
/// console really hands over (and which of them answer) is a hardware fact, and
/// this is what makes the next hardware run conclusive. A handle that was not
/// polled says so - "no readings" and "not asked" are different answers.
/// Returns the length written, 0 when `out` is unusable or no sensor was started.
size_t dglabJoyconDescribe(DglabJoyconSide side, char* out, size_t size);

/// Writes the Npad styles the console currently reports for player 1, e.g.
/// "0x00000004+joydual". Which style is active decides whether a side can be read
/// at all - a Joy-Con plugged back into the console switches the system to
/// `handheld` and the pair handles go quiet - and it is the one fact the readings
/// themselves cannot provide. Returns the length written, 0 when `out` is
/// unusable.
size_t dglabJoyconStyleText(char* out, size_t size);
