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

        text += used;

        memset(&glyph, 0, sizeof(glyph));

        if (!source->lookup(source, codepoint, &glyph)) {
            pen += missingAdvance(source);
            continue;
        }

        // Ink is stamped on whole pixels: the canvas has no alpha blending, and
        // half covered pixels would be the one place the UI looked smooth.
        for (int row = 0; row < glyph.height; row++) {
            const uint8_t* bits = glyph.bitmap + (size_t)row * (size_t)glyph.stride;

            for (int col = 0; col < glyph.width; col++) {
                if (bits[col >> 3] & (uint8_t)(0x80u >> (col & 7))) {
                    dglabCanvasFill(canvas, pen + glyph.bearing_x + col,
                        y + source->ascent - glyph.bearing_y + row, 1, 1, color);
                }
            }
        }

        pen += glyph.advance;
    }
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

    out->bitmap = font->glyphs + (size_t)index * (size_t)tile_bytes;
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
