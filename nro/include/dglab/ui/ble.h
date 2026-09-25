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
    u32 soft_limit;        ///< the ceiling this page asks for (0..100, step 1)
    int offset;            ///< first row shown, clamped by content height
} DglabBlePageState;

void dglabBleDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabBlePageState* state);

/// How tall the page's rows measure, so the caller can clamp its scroll offset
/// the same way the about page does.
int dglabBleContentHeight(const DglabFontSet* fonts, const DglabBlePageState* state);
