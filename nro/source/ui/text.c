#include <dglab/ui/text.h>

#include <string.h>

// Decodes one UTF-8 character. Returns the character and how many bytes it used;
// an invalid byte is returned as itself so the caller can advance by one.
static uint32_t decodeUtf8(const char* text, size_t* used)
{
    const unsigned char* p = (const unsigned char*)text;
    uint32_t codepoint;
    size_t length;

    if (p[0] < 0x80) {
        *used = 1;
        return p[0];
    }

    if ((p[0] & 0xE0) == 0xC0) {
        codepoint = (uint32_t)(p[0] & 0x1F);
        length = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        codepoint = (uint32_t)(p[0] & 0x0F);
        length = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        codepoint = (uint32_t)(p[0] & 0x07);
        length = 4;
    } else {
        *used = 1;
        return p[0];
    }

    for (size_t i = 1; i < length; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *used = 1;
            return p[0];
        }

        codepoint = (codepoint << 6) | (uint32_t)(p[i] & 0x3F);
    }

    *used = length;

    return codepoint;
}

// Width used for a character the font does not have: half a line, so a missing
// glyph shows up as a gap instead of glueing the neighbours together.
static int missingAdvance(DglabGlyphSource* source)
{
    return source->cell_height > 0 ? source->cell_height / 2 : 8;
}

void dglabTextDraw(DglabCanvas* canvas, DglabGlyphSource* source, int x, int y, const char* text,
    uint32_t color)
{
    int pen = x;
    size_t used = 0;

    if (!canvas || !source || !text)
        return;

    while (*text) {
        uint32_t codepoint = decodeUtf8(text, &used);
        DglabGlyph glyph;
        int left;
        int top;

        text += used;

        memset(&glyph, 0, sizeof(glyph));

        if (!source->lookup(source, codepoint, &glyph)) {
            pen += missingAdvance(source);
            continue;
        }

        // Coverage is blended, which is what makes the glyphs antialiased; the
        // bitmap font's 1-bit glyphs come through as full or no coverage.
        //
        // The pen walks in logical pixels - that is the unit the whole layout is
        // measured in and the unit dglabTextWidth reports - while the bitmap is
        // drawn at the buffer's own resolution: a scaled display rasterises the
        // font that much larger, so only the pen's position is converted here.
        left = dglabCanvasScale(canvas, pen + glyph.bearing_x);
        top = dglabCanvasScale(canvas, y + source->ascent - glyph.bearing_y);

        for (int row = 0; row < glyph.height; row++) {
            const uint8_t* bits = glyph.pixels + (size_t)row * (size_t)glyph.stride;

            for (int col = 0; col < glyph.width; col++) {
                uint8_t alpha = glyph.coverage ? bits[col]
                                               : (uint8_t)((bits[col >> 3] &
                                                     (uint8_t)(0x80u >> (col & 7))) ? 255u : 0u);

                if (alpha) {
                    dglabCanvasBlend(canvas, left + col, top + row, color, alpha);
                }
            }
        }

        pen += glyph.advance;
    }
}

// The ink of one line, relative to the y dglabTextDraw is given: `top` is the
// first row the glyphs cover and `bottom` is one past the last. An empty string
// or a line of missing glyphs has no ink at all.
static bool textInkBox(DglabGlyphSource* source, const char* text, int* top, int* bottom)
{
    size_t used = 0;
    bool any = false;

    if (!source || !text)
        return false;

    *top = 0;
    *bottom = 0;

    while (*text) {
        uint32_t codepoint = decodeUtf8(text, &used);
        DglabGlyph glyph;
        int glyph_top;
        int glyph_bottom;

        text += used;

        memset(&glyph, 0, sizeof(glyph));

        if (!source->lookup(source, codepoint, &glyph) || glyph.height <= 0)
            continue;

        glyph_top = source->ascent - glyph.bearing_y;
        glyph_bottom = glyph_top + glyph.height;

        if (!any || glyph_top < *top)
            *top = glyph_top;

        if (!any || glyph_bottom > *bottom)
            *bottom = glyph_bottom;

        any = true;
    }

    return any;
}

int dglabTextInkTop(DglabGlyphSource* source, const char* text)
{
    int top = 0;
    int bottom = 0;

    return textInkBox(source, text, &top, &bottom) ? top : 0;
}

int dglabTextInkHeight(DglabGlyphSource* source, const char* text)
{
    int top = 0;
    int bottom = 0;

    return textInkBox(source, text, &top, &bottom) ? bottom - top : 0;
}

int dglabTextWidth(DglabGlyphSource* source, const char* text)
{
    int width = 0;
    size_t used = 0;

    if (!source || !text)
        return 0;

    while (*text) {
        uint32_t codepoint = decodeUtf8(text, &used);
        DglabGlyph glyph;

        text += used;

        memset(&glyph, 0, sizeof(glyph));

        if (!source->lookup(source, codepoint, &glyph))
            width += missingAdvance(source);
        else
            width += glyph.advance;
    }

    return width;
}

size_t dglabTextWrapLine(DglabGlyphSource* source, const char* text, int max_width, char* out,
    size_t out_size)
{
    int width = 0;
    size_t offset = 0;
    size_t break_at = 0;
    size_t used = 0;

    if (!source || !text || !out || out_size == 0 || max_width <= 0)
        return 0;

    while (text[offset]) {
        uint32_t codepoint = decodeUtf8(text + offset, &used);

        if (used == 0)
            break;

        {
            DglabGlyph glyph;
            int advance;

            memset(&glyph, 0, sizeof(glyph));
            advance = source->lookup(source, codepoint, &glyph) ? glyph.advance
                                                                : missingAdvance(source);

            if (width + advance > max_width)
                break;

            width += advance;
        }

        offset += used;

        // Somewhere the line may end: after a space, or after anything that is
        // not an ASCII letter or digit (so "Joy-Con" and "100" stay whole).
        if (codepoint == ' ' || codepoint >= 0x80 ||
            !((codepoint >= '0' && codepoint <= '9') || (codepoint >= 'A' && codepoint <= 'Z') ||
              (codepoint >= 'a' && codepoint <= 'z')))
            break_at = offset;
    }

    // A line ends where the text really goes on: when everything that is left
    // fits, or when a single word is wider than the line, the cut is wherever
    // the characters stopped. Only a line that is full and has more text behind
    // it is cut at a break. (Using the last break seen in every case is what
    // turned the English sleep warning into three lines, one word each on the
    // last two.)
    if (text[offset] == '\0' || break_at == 0)
        break_at = offset;

    // Trailing spaces move to the next line instead of padding this one.
    while (break_at > 0 && text[break_at - 1] == ' ')
        break_at--;

    if (break_at >= out_size)
        break_at = out_size - 1;

    memcpy(out, text, break_at);
    out[break_at] = '\0';

    return break_at;
}

int dglabTextCountLines(DglabGlyphSource* source, const char* text, int max_width)
{
    int lines = 0;
    char line[192];

    if (!source || !text || max_width <= 0)
        return 0;

    while (*text) {
        size_t taken = dglabTextWrapLine(source, text, max_width, line, sizeof(line));

        if (taken == 0)
            break;

        lines++;
        text += taken;

        while (*text == ' ')
            text++;
    }

    return lines;
}

// ---------------------------------------------------------------------------
// The bitmap font adapter (ASCII)
// ---------------------------------------------------------------------------

static bool bitmapLookup(DglabGlyphSource* source, uint32_t codepoint, DglabGlyph* out)
{
    const DglabFont* font = source->context;
    int index = (int)codepoint - font->ascii_offset;
    int tile_bytes;
    int row_bytes;

    if (index < 0 || index >= font->glyph_count)
        return false;

    tile_bytes = font->tile_width * font->tile_height / 8;
    row_bytes = font->tile_width / 8;

    out->pixels = font->glyphs + (size_t)index * (size_t)tile_bytes;
    out->coverage = false;
    out->stride = row_bytes;
    out->width = font->tile_width;
    out->height = font->tile_height;
    out->bearing_x = 0;
    out->bearing_y = font->tile_height;
    out->advance = font->tile_width;

    return true;
}

static DglabGlyphSource g_bitmap_source;

DglabGlyphSource* dglabBitmapGlyphSource(const DglabFont* font)
{
    if (!font)
        return NULL;

    g_bitmap_source.lookup = bitmapLookup;
    g_bitmap_source.line_height = font->tile_height;
    g_bitmap_source.ascent = font->tile_height;
    g_bitmap_source.cell_height = font->tile_height;
    g_bitmap_source.context = (void*)font;

    return &g_bitmap_source;
}
