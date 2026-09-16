#pragma once

// The motion mode's screen (see docs/joycon-input.md). Same rules as the socket
// screen: the layout only reads a state struct, so tests/canvas can render it on
// a PC.

#include <dglab/ui/screen.h> // DglabCmdTone
#include <dglab/ui/text.h>

#include <stdbool.h>

typedef struct {
    bool left_connected;
    bool right_connected;
    bool moving_a; ///< the A side is currently above its dead zone
    bool moving_b;
    unsigned level_a;     ///< 0..100, what the mapping currently outputs
    unsigned level_b;
    unsigned frequency_a; ///< ms, follows the level
    unsigned frequency_b;

    // The channel strengths the socket screen set. They are the volume this
    // mode's waveform is multiplied with, so they belong on screen.
    unsigned channel_strength_a;
    unsigned channel_strength_b;

    // What the last upload answered, drawn like the socket screen's "last cmd".
    const char* link;      ///< e.g. "app connected" or "no app bound"
    u32 link_tone;         // DglabCmdTone
    const char* last_upload;
    u32 last_upload_tone; // DglabCmdTone

    // The server holds a listening socket while it runs, and the console hangs
    // if that happens across a sleep - the same warning the socket screen shows.
    bool server_running;
} DglabMotionScreenState;

void dglabMotionScreenDraw(DglabCanvas* canvas, DglabGlyphSource* text,
    const DglabMotionScreenState* state);
