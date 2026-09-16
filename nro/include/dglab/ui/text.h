#pragma once

// Text drawing that can show more than ASCII.
//
// The screens used to draw one byte per fixed 16x16 tile, which cannot express
// Chinese at all (a UTF-8 character is three bytes and would come out as three
// unrelated symbols). Here a glyph source hands out 1-bit glyphs - either from a
// bitmap font or rasterised on demand from a TTF - and this layer decodes UTF-8,
// measures and blits them on whole pixels, so the result keeps the hard edged
// look the UI already has (docs/nro-ui.md).

#include <dglab/ui/canvas.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t* bitmap; ///< 1 bit per pixel, row major, MSB is the leftmost
    int stride;            ///< bytes per row of the bitmap
    int width;             ///< ink box
    int height;
    int bearing_x;         ///< pen to ink box left
    int bearing_y;         ///< baseline up to ink box top
    int advance;           ///< pen movement after the glyph
} DglabGlyph;

typedef struct DglabGlyphSource DglabGlyphSource;

struct DglabGlyphSource {
    /// Looks one code point up. Returns false when the font has no glyph for it.
    bool (*lookup)(DglabGlyphSource* source, uint32_t codepoint, DglabGlyph* out);
    int line_height; ///< baseline to baseline
    int ascent;      ///< top of the line down to the baseline
    int cell_height; ///< ink height of one line, for sizing rows
    void* context;
};

/// Draws text with its top left at (x, y). Missing glyphs are skipped, and the
/// pen still moves, so a string never collapses onto itself.
void dglabTextDraw(DglabCanvas* canvas, DglabGlyphSource* source, int x, int y, const char* text,
    uint32_t color);

/// The width dglabTextDraw would use, in pixels.
int dglabTextWidth(DglabGlyphSource* source, const char* text);

/// A source over libnx's bitmap font (ASCII only). Used on the host, where there
/// is no system font to rasterise, and as the fallback when the real font cannot
/// be loaded on the console.
DglabGlyphSource* dglabBitmapGlyphSource(const DglabFont* font);
