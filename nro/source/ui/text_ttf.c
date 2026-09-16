#include <dglab/ui/text_ttf.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

#include <math.h>
#include <string.h>

// Glyphs are cached: rasterising every character of every frame would be the one
// place this UI could get slow, and a screen only ever uses a few dozen.
#define CACHE_SLOTS 512
#define MAX_GLYPH_PIXELS 64

// Coverage above this becomes ink. Chosen so thin strokes survive at 24px; the
// result is 1 bit, not grey (see docs/nro-ui.md).
#define INK_THRESHOLD 140

typedef struct {
    uint32_t codepoint; // 0 marks an unused slot
    DglabGlyph glyph;
    uint8_t bitmap[MAX_GLYPH_PIXELS * MAX_GLYPH_PIXELS / 8];
} CacheEntry;

static stbtt_fontinfo g_font;
static float g_scale;
static bool g_loaded;
static DglabGlyphSource g_source;
static CacheEntry g_cache[CACHE_SLOTS];
static unsigned g_next_slot;

static bool ttfLookup(DglabGlyphSource* source, uint32_t codepoint, DglabGlyph* out)
{
    CacheEntry* entry = NULL;
    int glyph_index;
    int advance = 0, bearing = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width, height;
    uint8_t coverage[MAX_GLYPH_PIXELS * MAX_GLYPH_PIXELS];

    (void)source;

    if (!g_loaded)
        return false;

    for (unsigned i = 0; i < CACHE_SLOTS; i++) {
        if (g_cache[i].codepoint == codepoint) {
            entry = &g_cache[i];
            break;
        }
    }

    if (entry == NULL) {
        entry = &g_cache[g_next_slot];
        g_next_slot = (g_next_slot + 1u) % CACHE_SLOTS;

        memset(entry, 0, sizeof(*entry));
        entry->codepoint = codepoint;

        glyph_index = stbtt_FindGlyphIndex(&g_font, (int)codepoint);

        if (glyph_index == 0 && codepoint != ' ')
            return false;

        stbtt_GetGlyphHMetrics(&g_font, glyph_index, &advance, &bearing);
        stbtt_GetGlyphBitmapBox(&g_font, glyph_index, g_scale, g_scale, &x0, &y0, &x1, &y1);

        width = x1 - x0;
        height = y1 - y0;

        entry->glyph.advance = (int)ceilf((float)advance * g_scale);
        entry->glyph.bearing_x = x0;
        entry->glyph.bearing_y = -y0;
        entry->glyph.width = width > 0 ? width : 0;
        entry->glyph.height = height > 0 ? height : 0;
        entry->glyph.stride = (entry->glyph.width + 7) / 8;
        entry->glyph.bitmap = entry->bitmap;

        if (entry->glyph.width > MAX_GLYPH_PIXELS)
            entry->glyph.width = MAX_GLYPH_PIXELS;

        if (entry->glyph.height > MAX_GLYPH_PIXELS)
            entry->glyph.height = MAX_GLYPH_PIXELS;

        if (width > 0 && height > 0 && width <= MAX_GLYPH_PIXELS && height <= MAX_GLYPH_PIXELS) {
            stbtt_MakeGlyphBitmap(&g_font, coverage, width, height, width, g_scale, g_scale,
                glyph_index);

            for (int row = 0; row < height; row++) {
                for (int col = 0; col < width; col++) {
                    if (coverage[row * width + col] > INK_THRESHOLD) {
                        entry->bitmap[((size_t)row * (size_t)entry->glyph.stride) + (size_t)(col >> 3)] |=
                            (uint8_t)(0x80u >> (col & 7));
                    }
                }
            }
        }
    }

    *out = entry->glyph;

    return true;
}

bool dglabTtfFontInit(const void* data, size_t size, float pixel_height)
{
    int ascent = 0, descent = 0, line_gap = 0;
    int offset;

    g_loaded = false;
    memset(g_cache, 0, sizeof(g_cache));
    g_next_slot = 0;

    if (!data || size == 0)
        return false;

    offset = stbtt_GetFontOffsetForIndex((const unsigned char*)data, 0);

    if (offset < 0)
        return false;

    if (!stbtt_InitFont(&g_font, (const unsigned char*)data, offset))
        return false;

    g_scale = stbtt_ScaleForPixelHeight(&g_font, pixel_height);
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &line_gap);

    g_source.lookup = ttfLookup;
    g_source.ascent = (int)ceilf((float)ascent * g_scale);
    g_source.line_height = (int)ceilf((float)(ascent - descent + line_gap) * g_scale);
    g_source.cell_height = g_source.ascent + (int)ceilf((float)-descent * g_scale);
    g_source.context = NULL;

    g_loaded = true;

    return true;
}

DglabGlyphSource* dglabTtfFontSource(void)
{
    return g_loaded ? &g_source : NULL;
}
