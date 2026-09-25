#pragma once

// The Bluetooth page: the dedicated screen for driving the device directly.
//
// The sysmodule owns the session (it is the only component that may hold the
// connection); this page starts it, stops it, and shows what BLE_STATUS reports.
// Starting is two steps because the transport needs two: the driver-level probe
// brings the Bluetooth stack up, then the session connects (docs/ble-poc.md).
//
// What the page shows about strength is honest about being open loop: the device
// never reports back on this firmware (docs/ble-re.md, "连接所有权在服务层是封的"),
// so the row shows the value this side asked for and the note says so.
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
    /// the shared settings (0..100). Not the strengths below them.
    u32 limit_a;
    u32 limit_b;
    /// The two strengths, the same pair the socket and gameplay pages dial.
    u32 strength_a;
    u32 strength_b;
    /// The BLE log is its own page, opened and closed with Y, the way the socket
    /// page's log is (docs/nro-ui.md).
    bool log_open;
    int log_offset;
    int offset;            ///< first row shown, clamped by content height
} DglabBlePageState;

void dglabBleDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabBlePageState* state);

/// How tall the page's rows measure, so the caller can clamp its scroll offset
/// the same way the about page does.
int dglabBleContentHeight(const DglabFontSet* fonts, const DglabBlePageState* state);
