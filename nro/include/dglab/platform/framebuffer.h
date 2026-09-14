#pragma once

// libnx framebuffer backend for the NRO canvas.
//
// This is the only file in the NRO that touches the display; see
// docs/nro-ui.md for why the framebuffer was chosen and how a deko3d backend
// would slot in behind the same canvas interface.

#include <dglab/ui/canvas.h>

#include <stdbool.h>

// Creates the window framebuffer. Returns false when it could not be created.
bool dglabFramebufferOpen(void);
void dglabFramebufferClose(void);

// Returns the canvas for the next frame, or false when no buffer is free.
bool dglabFramebufferBegin(DglabCanvas* canvas);

// Hands the finished frame to the display.
void dglabFramebufferEnd(void);

// The system font libnx ships for its console: 16x16 tiles, 256 characters.
const DglabFont* dglabFramebufferFont(void);
