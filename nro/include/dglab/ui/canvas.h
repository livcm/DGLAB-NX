#pragma once

// Small software canvas for the NRO.
//
// Everything here draws into a plain RGBA8888 buffer and stays free of libnx,
// so the layout code is testable on a PC (tests/canvas). The Switch backend in
// nro/source/platform/framebuffer.c only creates the buffer and presents it;
// swapping it for a deko3d backend later means reimplementing that one file
// (see docs/nro-ui.md).

#include <dglab/ui/qr.h>

#include <stdbool.h>
#include <stdint.h>

#define DGLAB_RGBA(r, g, b, a) \
    (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))

typedef struct {
    uint8_t* pixels; // RGBA8888, row major
    int width;
    int height;
    int stride; // bytes per row, may be larger than width * 4

    // On top of the buffer bounds, drawing is confined to this rectangle. It
    // starts as the whole canvas; a page narrows it to its content column
    // (dglabPageClipContent) so a row scrolled past the edge is cut there
    // instead of drawn over the rules.
    int clip_x;
    int clip_y;
    int clip_width;
    int clip_height;
} DglabCanvas;

// Bitmap font. The glyph data is one bit per pixel, tile_width must be a
// multiple of 8, and a row is a little endian value whose most significant bit
// is the leftmost pixel. That is the layout of libnx's default font (16x16
// tiles, 32 bytes per glyph, as read by libnx's own console renderer).
typedef struct {
    const uint8_t* glyphs;
    int ascii_offset;
    int glyph_count;
    int tile_width;
    int tile_height;
} DglabFont;

void dglabCanvasInit(DglabCanvas* canvas, uint8_t* pixels, int width, int height, int stride);

// Blends one pixel: alpha 0 keeps what is there, 255 replaces it. This is what
// glyph coverage needs - everything else in the canvas simply overwrites.
void dglabCanvasBlend(DglabCanvas* canvas, int x, int y, uint32_t color, uint8_t alpha);

// All drawing clips to the canvas, so a layout mistake cannot corrupt memory.
void dglabCanvasFill(DglabCanvas* canvas, int x, int y, int width, int height, uint32_t color);
void dglabCanvasFrame(DglabCanvas* canvas, int x, int y, int width, int height, int thickness,
    uint32_t color);

// The one pixel rule the console draws between list rows and under the header.
// It is a fill of height 1, but the name keeps the screens from spelling out a
// height that a later scale would have to change in twenty places.
void dglabCanvasHLine(DglabCanvas* canvas, int x, int y, int width, uint32_t color);

// Rounded rectangle, filled (`RoundFill`) and stroked (`RoundFrame`). The focus
// box of every row in the console's UI is a stroked one of these, so its radius
// and thickness are most of what makes the style recognisable (docs/nro-ui.md).
void dglabCanvasRoundFill(DglabCanvas* canvas, int x, int y, int width, int height, int radius,
    uint32_t color);
void dglabCanvasRoundFrame(DglabCanvas* canvas, int x, int y, int width, int height, int radius,
    int thickness, uint32_t color);

// Filled disc and ring: the slider knob is the first and the footer's button
// hints are the second ("A" inside a ring). Both are antialiased at the edge.
void dglabCanvasDisc(DglabCanvas* canvas, int cx, int cy, int radius, uint32_t color);
void dglabCanvasRing(DglabCanvas* canvas, int cx, int cy, int radius, int thickness,
    uint32_t color);

// Confines every later drawing call to `width x height` at (x, y), intersected
// with the canvas. dglabCanvasClearClip() gives the whole canvas back.
void dglabCanvasSetClip(DglabCanvas* canvas, int x, int y, int width, int height);
void dglabCanvasClearClip(DglabCanvas* canvas);

// Draws text with the given pixel scale. Unsupported characters are drawn as
// blanks.
void dglabCanvasText(DglabCanvas* canvas, const DglabFont* font, int x, int y, int scale,
    const char* text, uint32_t color);

// Width one line of text occupies, in pixels.
int dglabCanvasTextWidth(const DglabFont* font, int scale, const char* text);

// Draws the symbol with a quiet zone, dark modules in `dark` and light modules
// in `light` (passing 0 for light leaves the background alone).
void dglabCanvasQr(DglabCanvas* canvas, const DglabQrCode* code, int x, int y, int module_size,
    int quiet_zone, uint32_t dark, uint32_t light);
