#pragma once

// The touch mode's screen (see docs/touch-input.md). Same rules as the socket
// and motion screens: the layout only reads a state struct, so tests/canvas can
// render it on a PC.
//
// This page is the one exception to the content column: the whole band between
// the two white rules is the play field, because the field *is* the input - the
// page header and the bottom bar keep their places, and everything between them
// belongs to the two halves (nro/AGENTS.md).

#include <dglab/nro/touch_panel.h>
#include <dglab/ui/screen.h> // DglabCmdTone
#include <dglab/ui/text.h>

#include <stdbool.h>

typedef struct {
    /// The console is in the dock, where the panel cannot be reached at all. The
    /// page says so instead of looking broken, and stays usable with the pad.
    bool docked;

    /// Which half has a finger in it, where the panel says it is (panel
    /// coordinates, which are the screen's own logical units), and what the mode
    /// makes of that position. The value and the interval are the smoothed ones
    /// the slots actually carry, not the raw axes: what is being uploaded is what
    /// the page has to show.
    bool held_a;
    bool held_b;
    unsigned x_a;
    unsigned y_a;
    unsigned x_b;
    unsigned y_b;
    unsigned level_a;     ///< 0..100 waveform value
    unsigned level_b;
    unsigned frequency_a; ///< ms between pulses, from the horizontal axis
    unsigned frequency_b;

    // The channel strengths the socket screen set. They are the volume this
    // mode's waveform is multiplied with, so they belong on screen - the same
    // two rows the motion page carries.
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

    /// The title bar's right hand side: whether the sysmodule still answers.
    bool sysmodule_ok;
} DglabTouchScreenState;

void dglabTouchScreenDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabTouchScreenState* state);
