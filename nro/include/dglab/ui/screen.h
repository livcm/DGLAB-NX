#pragma once

// Layout of the socket server screen.
//
// Everything the screen needs comes in through DglabScreenState, so the same
// code runs on the console (through the framebuffer backend) and on the host
// (tests/canvas renders it into a plain buffer to check the layout).

#include <dglab/ipc.h>
#include <dglab/ui/canvas.h>

#include <stdbool.h>

#define DGLAB_SCREEN_LOG_LINES 12
#define DGLAB_SCREEN_LOG_LINE_LEN 40

typedef struct {
    DglabIpcVersion version;
    bool status_ok;
    DglabNetStatus status;

    bool url_ok;      // a QR payload is available
    const char* url;  // QR payload, see docs/dglab-socket.md

    u32 test_strength;

    const char* const* log_lines;
    int log_count;
} DglabScreenState;

void dglabScreenDraw(DglabCanvas* canvas, const DglabFont* font, const DglabScreenState* state);
