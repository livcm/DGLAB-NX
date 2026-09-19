#pragma once

// The touch panel, and the only file in this NRO that talks to libnx's HID touch
// screen API (see docs/touch-input.md).
//
// Three facts about that API shape this layer:
//
//   - hidInitializeTouchScreen() returns void. It sends hid's own command with
//     this process's applet resource user id, and when the console refuses it
//     calls libnx's diagAbortWithResult - the fatal error page. There is no
//     Result to check and no way to carry on without a panel, so "can this build
//     read the touch screen at all" is a hardware question, and the first thing
//     to verify on hardware is that entering the mode does not land there.
//   - readings live in a shared memory LIFO of 17 snapshots, like the six-axis
//     sensor: a frame that takes only the newest snapshot loses whatever
//     happened while it was busy. Polling drains the LIFO and merges the press
//     and release edges into the newest snapshot.
//   - the panel can only be reached in handheld mode. A docked console reports
//     no touches at all, which is why the mode says so instead of looking
//     broken.
//
// The mode is the only caller: the touch screen is read while the touch mode is
// on screen and at no other time (nro/AGENTS.md).

#include <dglab/nro/touch_panel.h>

#include <stddef.h>

/// Enters the mode: initializes the touch screen, once per process. Nothing is
/// read back - see the note above about libnx's error path.
void dglabTouchStart(void);

/// Drains the LIFO and fills `frame` with the newest snapshot plus the edges the
/// drained snapshots carried. Returns how many snapshots were drained, which is
/// 0 when the panel had nothing new; the count is a hardware fact worth logging,
/// the same way the motion mode logs the six-axis LIFO's depth.
size_t dglabTouchPoll(DglabTouchFrame* frame);

/// One line describing the last poll - how deep the LIFO was, how many touches
/// the newest snapshot carried, and where the first few of them were - so the
/// probe run can be read off the SD card's log instead of off the screen.
/// Returns the length written, 0 when nothing was polled yet.
size_t dglabTouchDescribe(char* out, size_t size);

/// Whether the console is being used handheld. The panel cannot be reached while
/// it sits in the dock.
bool dglabTouchHandheld(void);
