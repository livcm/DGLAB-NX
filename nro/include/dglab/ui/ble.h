#pragma once

// The Bluetooth page: the dedicated screen for driving the device directly.
//
// The sysmodule owns the session (it is the only component that may hold the
// connection); this page starts it, stops it, and shows what BLE_STATUS reports.
// Starting is two steps because the transport needs two: the driver-level probe
// brings the Bluetooth stack up, then the session connects (docs/ble-poc.md).
//
// The page is the two channel strengths and what the session is doing, nothing
// else: the two channel ceilings are settings (the parameters page owns them) and
// they are already readable as the ceiling half of each strength row, so the page
// does not repeat them as rows, and it carries no text blocks - the two
// adjustment hints are the line above the rows, the same line every other
// gameplay page has, and the page fits one screen because up and down belong to
// channel A rather than to a scroll. What the rows cannot say (the device never
// reports back on this firmware, so these are the values this side asked for, see
// docs/ble-re.md) is in the mode's description on the menu.
//
// Strength is dialled here exactly the way the socket and gameplay pages dial
// it (up/down channel A, left/right channel B, hold to walk), because the
// sysmodule routes NET_SEND to the BLE session while one runs. The two channel
// strength *ceilings* are not adjusted here: they are settings (the advanced
// parameters page owns them, see motion_settings.h) and this page only shows
// them, so "the cap" has one home and "the strength" has another.

#include <dglab/ipc.h>
#include <dglab/ui/canvas.h>
#include <dglab/ui/list.h>
#include <dglab/ui/text.h>

#include <stdbool.h>

typedef struct {
    DglabBleStatus status; ///< what the last BLE_STATUS reported
    bool sysmodule_ok;     ///< the header's liveness line
    bool driver_running;   ///< step 1 of the start sequence is still running
    bool starting;         ///< ...and step 2 has not been sent yet
    /// The two channel strength ceilings the session is started with, read from
    /// the shared settings (0..100). Not the strengths below them: they are the
    /// ceiling half of those two rows, and the value the D-pad stops at.
    u32 limit_a;
    u32 limit_b;
    /// The two strengths, the same pair the socket and gameplay pages dial.
    u32 strength_a;
    u32 strength_b;
    /// The BLE log is its own page, opened and closed with Y, the way the socket
    /// page's log is (docs/nro-ui.md).
    bool log_open;
    int log_offset;
} DglabBlePageState;

void dglabBleDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabBlePageState* state);
