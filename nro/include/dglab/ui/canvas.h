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

// All drawing clips to the canvas, so a layout mistake cannot corrupt memory.
void dglabCanvasFill(DglabCanvas* canvas, int x, int y, int width, int height, uint32_t color);
void dglabCanvasFrame(DglabCanvas* canvas, int x, int y, int width, int height, int thickness,
    uint32_t color);

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
