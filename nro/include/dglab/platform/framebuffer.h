#pragma once

// libnx framebuffer backend for the NRO canvas.
//
// This is the only file in the NRO that touches the display; see
// docs/nro-ui.md for why the framebuffer was chosen and how a deko3d backend
// would slot in behind the same canvas interface.

#include <dglab/ui/canvas.h>

#include <stdbool.h>

// Creates the window framebuffer at the resolution the console is running at:
// 1280x720 in handheld, 1920x1080 in docked mode, which is what makes the
// docked picture native instead of the system's upscale of a 720p frame
// (docs/nro-ui.md). A docked console whose 1080p frame cannot be created falls
// back to 720p, which is what this build always did. Returns false when neither
// could be created.
bool dglabFramebufferOpen(void);
void dglabFramebufferClose(void);

// Returns the canvas for the next frame, or false when no buffer is free.
bool dglabFramebufferBegin(DglabCanvas* canvas);

// Hands the finished frame to the display.
void dglabFramebufferEnd(void);

// The system font libnx ships for its console: 16x16 tiles, 256 characters.
const DglabFont* dglabFramebufferFont(void);

// Physical pixels per logical pixel of the open framebuffer: 1/1 in handheld,
// 3/2 with a 1080p frame. The canvas takes it from here
// (dglabFramebufferBegin), and so do the font sizes, so that a screen's own
// coordinates stay in the 720p system it was laid out in.
void dglabFramebufferScale(int* num, int* den);
