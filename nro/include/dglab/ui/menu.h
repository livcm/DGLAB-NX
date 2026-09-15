#pragma once

// The mode menu the NRO opens with (see docs/joycon-input.md). Drawing only, so
// tests/canvas can render it on a PC like the rest of the layout.

#include <dglab/ui/canvas.h>

#include <stdbool.h>

typedef enum {
    DglabMenu_ItemSocket = 0, ///< server, QR code, test buttons, log
    DglabMenu_ItemMotion,     ///< Joy-Con driven waveform
    DglabMenu_ItemBlePoc,     ///< the abandoned BLE console, kept for diagnostics
    DglabMenu_ItemCount,
} DglabMenuItem;

typedef struct {
    unsigned selected;   ///< DglabMenuItem
    bool sysmodule_ok;   ///< the last ping was answered
} DglabMenuState;

void dglabMenuDraw(DglabCanvas* canvas, const DglabFont* font, const DglabMenuState* state);

/// Moves the selection by `delta` and wraps around, so the caller does not have
/// to do signed modulo arithmetic on an unsigned index.
unsigned dglabMenuMove(unsigned selected, int delta);

/// Short name shown in the list.
const char* dglabMenuItemName(unsigned item);

/// One or two sentences about what the mode does, shown under the list.
const char* dglabMenuItemDescription(unsigned item);
