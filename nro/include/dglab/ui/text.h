#pragma once

// Text drawing that can show more than ASCII.
//
// The screens used to draw one byte per fixed 16x16 tile, which cannot express
// Chinese at all (a UTF-8 character is three bytes and would come out as three
// unrelated symbols). Here a glyph source hands out glyphs - coverage from a
// rasterised TTF, or libnx's 1-bit bitmap font - and this layer decodes UTF-8,
// measures and blends them, so text can be drawn antialiased (docs/nro-ui.md).

#include <dglab/ui/canvas.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t* pixels; ///< coverage (8 bit) or one bit per pixel, see below
    bool coverage;         ///< true: one byte per pixel, 0..255; false: 1 bit, MSB left
    int stride;            ///< bytes per row of the bitmap
    // The ink box, in the buffer's pixels: `width`/`height` size the bitmap
    // itself, which a scaled display rasterises larger than the logical box the
    // metrics above describe (see docs/nro-ui.md).
    int width;
    int height;
    int bearing_x;         ///< pen to ink box left, in logical pixels
    int bearing_y;         ///< baseline up to ink box top, in logical pixels
    int advance;           ///< pen movement after the glyph, in logical pixels
} DglabGlyph;

typedef struct DglabGlyphSource DglabGlyphSource;

// The four sizes the console's UI draws at 720p, measured off system settings
// screenshots (docs/nro-ui.md): the page title, the rows, the values on the
// right of a row, and the small grey explanation under one. Every screen picks
// from these instead of inventing a size, which is what keeps the pages looking
// like one system.
#define DGLAB_TEXT_TITLE 28
#define DGLAB_TEXT_BODY 24
#define DGLAB_TEXT_VALUE 22
#define DGLAB_TEXT_NOTE 18
/// The letter knocked out of a button icon. Smaller than the row text so the
/// letter has room inside its shape instead of touching the outline
/// (docs/nro-ui.md has the measurements).
#define DGLAB_TEXT_ICON 20

/// A font together with the colour it is drawn in. The components take these
/// rather than a pair of arguments each, so a screen can describe a row without
/// repeating the theme lookup.
typedef struct {
    DglabGlyphSource* font;
    uint32_t color;
} DglabTextStyle;

/// The four sizes one face is rasterised at, as one bundle: a screen takes this
/// and picks the size a piece of text calls for. The platform layer fills it in
/// from the console's shared font, the host tools from a file or from the
/// bitmap font.
typedef struct {
    DglabGlyphSource* title; ///< DGLAB_TEXT_TITLE
    DglabGlyphSource* body;  ///< DGLAB_TEXT_BODY
    DglabGlyphSource* value; ///< DGLAB_TEXT_VALUE
    DglabGlyphSource* note;  ///< DGLAB_TEXT_NOTE
    DglabGlyphSource* icon;  ///< DGLAB_TEXT_ICON
} DglabFontSet;

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

/// Where the ink starts below the y dglabTextDraw is given, and how tall it is.
/// A button centres its letter with these rather than with the line box: the box
/// carries the descender space under the baseline, which leaves a letter that
/// sits on the baseline visibly high in its shape.
int dglabTextInkTop(DglabGlyphSource* source, const char* text);
int dglabTextInkHeight(DglabGlyphSource* source, const char* text);

/// Takes one line's worth of `text` that fits `max_width` pixels, writes it into
/// out (without any trailing space) and returns how many bytes of `text` that
/// consumed. It prefers to break at a space, but Chinese has none, so it also
/// breaks between characters - except inside a run of ASCII letters or digits,
/// which are kept together unless the run is wider than the line, in which case
/// it ends at the line's edge rather than running past it. A line is only cut
/// when the text goes on: the last line of a paragraph is never shortened.
/// Returns 0 only for an empty string or no room.
size_t dglabTextWrapLine(DglabGlyphSource* source, const char* text, int max_width, char* out,
    size_t out_size);

/// How many wrapped lines `text` needs at this width. Panels size themselves
/// from this instead of a hand computed guess, which is what let a border cut
/// through the last row of the about screen.
int dglabTextCountLines(DglabGlyphSource* source, const char* text, int max_width);

/// A source over libnx's bitmap font (ASCII only). Used on the host, where there
/// is no system font to rasterise, and as the fallback when the real font cannot
/// be loaded on the console.
DglabGlyphSource* dglabBitmapGlyphSource(const DglabFont* font);
