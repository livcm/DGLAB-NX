#pragma once

// The socket server page.
//
// Everything the screen needs comes in through DglabScreenState, so the same
// code runs on the console (through the framebuffer backend) and on the host
// (tests/canvas renders it into a plain buffer to check the layout).
//
// The page is a list of rows - server, address, the two channels with their
// sliders, the rest of the counters - followed by the QR code and the sysmodule
// log, and it scrolls, which is what the console's own pages do (docs/nro-ui.md).

#include <dglab/ipc.h>
#include <dglab/ui/canvas.h>
#include <dglab/ui/text.h>

#include <stdbool.h>

/// The ring the front end keeps of the sysmodule's log. It is the log page's
/// whole content, and the page scrolls, so it is worth more than one screenful
/// (the SD card mirror keeps everything).
#define DGLAB_SCREEN_LOG_LINES 32
#define DGLAB_SCREEN_LOG_LINE_LEN 40
/// The log page's line pitch: the console's long text pages use a wider one than
/// a list row (docs/nro-ui.md).
#define DGLAB_SCREEN_LOG_PITCH 37
/// The two columns of the socket page: the code and its counters on the left,
/// the parameters on the right. These are the console's own two column numbers -
/// the left band starts at 80 and the right one runs from 470 to 1189 - not the
/// narrower content column a one column page uses (docs/nro-ui.md).
#define DGLAB_SOCKET_QR_X 80
#define DGLAB_SOCKET_QR_WIDTH 390
#define DGLAB_SOCKET_INFO_X 470
#define DGLAB_SOCKET_INFO_WIDTH 719

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

    /// The sysmodule log is its own page, opened and closed with Y. Nothing on
    /// this screen is focused: every action is a shortcut, so there is no cursor
    /// to draw and no row to scroll to.
    bool log_open;
    /// The first log line the log page shows; 0 is the oldest. main.c keeps it
    /// at the newest line when the page opens and clamps it afterwards.
    int log_offset;
} DglabScreenState;

void dglabScreenDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabScreenState* state);
