#pragma once

// Layout of the socket server screen.
//
// Everything the screen needs comes in through DglabScreenState, so the same
// code runs on the console (through the framebuffer backend) and on the host
// (tests/canvas renders it into a plain buffer to check the layout).

#include <dglab/ipc.h>
#include <dglab/ui/canvas.h>
#include <dglab/ui/text.h>

#include <stdbool.h>

#define DGLAB_SCREEN_LOG_LINES 12
#define DGLAB_SCREEN_LOG_LINE_LEN 40
// How many of the ring the panel shows: 16px lines, and the panel only has room
// for this many above the footer (the SD card gets all of them).
#define DGLAB_SCREEN_LOG_VISIBLE 9

// How the "last cmd" line is painted: it worked, it worked but there is nothing
// to hear, or the sysmodule refused it.
typedef enum {
    DglabCmdTone_Ok = 0,
    DglabCmdTone_Warn,
    DglabCmdTone_Error,
} DglabCmdTone;

typedef struct {
    DglabIpcVersion version;
    bool status_ok;
    DglabNetStatus status;

    bool url_ok;      // a QR payload is available
    const char* url;  // QR payload, see docs/dglab-socket.md

    // Channel strengths the test buttons send and the D-pad changes, raw device
    // values (0..100), one per channel.
    u32 test_strength_a;
    u32 test_strength_b;


    // What the buttons last sent and what the sysmodule answered, e.g.
    // "A test  ok (A is 0)" or "clear  no app bound". Built in main.c, where
    // libnx's Result values are available; empty until the first command.
    const char* last_command;
    u32 last_command_tone; // DglabCmdTone

    const char* const* log_lines;
    int log_count;
} DglabScreenState;

/// The screen takes two text sources on purpose: the status panel, the QR panel
/// and the footer use the localised font, while the sysmodule log keeps libnx's
/// 16px bitmap font so that twelve dense lines still fit on screen
/// (docs/nro-ui.md).
void dglabScreenDraw(DglabCanvas* canvas, DglabGlyphSource* text, const DglabFont* log_font,
    const DglabScreenState* state);
