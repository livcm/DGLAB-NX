#include <dglab/ui/text_ttf.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

#include <math.h>
#include <string.h>

// Glyphs are cached per size: rasterising every character of every frame would
// be the one place this UI could get slow. A big screen uses a couple of hundred
// distinct characters, so the cache holds a page's worth and the ring eviction
// only costs a re-rasterisation.
//
// The bitmap is sized for the *physical* box: a docked console rasterises the
// 28px title at 42px, and a CJK glyph's ink can be a little taller than its em.
// A box that is too small does not clip the glyph, it drops it - the cache
// clamps the box and then skips the rasterisation - so MAX_GLYPH_PIXELS is
// checked below against the largest size the UI draws and the largest scale it
// uses.
//
// Physical pixels per logical pixel, as used by the largest scale the NRO asks
// for (docked 1080p).
#define TTF_MAX_SCALE_NUM 3
#define TTF_MAX_SCALE_DEN 2

#define CACHE_SLOTS 128
#define MAX_GLYPH_PIXELS 48

_Static_assert(MAX_GLYPH_PIXELS >= (int)(DGLAB_TEXT_TITLE * TTF_MAX_SCALE_NUM /
    TTF_MAX_SCALE_DEN) + 6,
    "MAX_GLYPH_PIXELS must cover the title rasterised at the largest display scale");

typedef struct {
    uint32_t codepoint; // 0 marks an unused slot
    DglabGlyph glyph;
    uint8_t bitmap[MAX_GLYPH_PIXELS * MAX_GLYPH_PIXELS];
} CacheEntry;

struct DglabTtfFont {
    stbtt_fontinfo info;
    float scale;      ///< em size in buffer pixels, for stb_truetype
    int metric_num;   ///< buffer pixels per logical pixel, for the metrics
    int metric_den;
    bool loaded;
    DglabGlyphSource source;
    CacheEntry cache[CACHE_SLOTS];
    unsigned next_slot;
};

// A buffer pixel count back in logical pixels, never rounding a visible glyph
// down to nothing.
static int toLogical(const struct DglabTtfFont* font, int value)
{
    int scaled;

    if (value == 0)
        return 0;

    scaled = value * font->metric_den;
    scaled = scaled >= 0 ? (scaled + font->metric_num / 2) / font->metric_num
                         : -((-scaled + font->metric_num / 2) / font->metric_num);

    if (scaled == 0)
        scaled = value > 0 ? 1 : -1;

    return scaled;
}

static struct DglabTtfFont g_fonts[DGLAB_TTF_MAX_SIZES];
static unsigned g_font_count;

static bool ttfLookup(DglabGlyphSource* source, uint32_t codepoint, DglabGlyph* out)
{
    struct DglabTtfFont* font = source->context;
    CacheEntry* entry = NULL;
    int glyph_index;
    int advance = 0, bearing = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width, height;

    if (font == NULL || !font->loaded)
        return false;

    for (unsigned i = 0; i < CACHE_SLOTS; i++) {
        if (font->cache[i].codepoint == codepoint) {
            entry = &font->cache[i];
            break;
        }
    }

    if (entry == NULL) {
        entry = &font->cache[font->next_slot];
        font->next_slot = (font->next_slot + 1u) % CACHE_SLOTS;

        memset(entry, 0, sizeof(*entry));
        entry->codepoint = codepoint;

        glyph_index = stbtt_FindGlyphIndex(&font->info, (int)codepoint);

        if (glyph_index == 0 && codepoint != ' ')
            return false;

        stbtt_GetGlyphHMetrics(&font->info, glyph_index, &advance, &bearing);
        stbtt_GetGlyphBitmapBox(&font->info, glyph_index, font->scale, font->scale, &x0, &y0, &x1,
            &y1);

        width = x1 - x0;
        height = y1 - y0;

        // The metrics are the layout's, so they are logical; only the ink box
        // below stays in buffer pixels, because that is the bitmap's own size.
        entry->glyph.advance = toLogical(font, (int)ceilf((float)advance * font->scale));
        entry->glyph.bearing_x = toLogical(font, x0);
        entry->glyph.bearing_y = toLogical(font, -y0);
        entry->glyph.width = width > 0 ? width : 0;
        entry->glyph.height = height > 0 ? height : 0;
        entry->glyph.stride = entry->glyph.width;
        entry->glyph.pixels = entry->bitmap;
        entry->glyph.coverage = true;

        if (entry->glyph.width > MAX_GLYPH_PIXELS)
            entry->glyph.width = MAX_GLYPH_PIXELS;

        if (entry->glyph.height > MAX_GLYPH_PIXELS)
            entry->glyph.height = MAX_GLYPH_PIXELS;

        if (width > 0 && height > 0 && width <= MAX_GLYPH_PIXELS && height <= MAX_GLYPH_PIXELS) {
            // Antialiased: the coverage goes to the canvas as it is. The one bit
            // version looked too rough on hardware (docs/nro-ui.md).
            stbtt_MakeGlyphBitmap(&font->info, entry->bitmap, width, height, width, font->scale,
                font->scale, glyph_index);
        }
    }

    *out = entry->glyph;

    return true;
}

static bool prepare(struct DglabTtfFont* font, const void* data, size_t size, float pixel_height)
{
    int ascent = 0, descent = 0, line_gap = 0;
    int offset;

    font->loaded = false;
    memset(font->cache, 0, sizeof(font->cache));
    font->next_slot = 0;

    if (!data || size == 0)
        return false;

    offset = stbtt_GetFontOffsetForIndex((const unsigned char*)data, 0);

    if (offset < 0)
        return false;

    if (!stbtt_InitFont(&font->info, (const unsigned char*)data, offset))
        return false;

    // The em is rasterised at the scale the display is running at; everything
    // the layout reads is converted back to logical pixels below.
    font->scale = stbtt_ScaleForPixelHeight(&font->info,
        pixel_height * (float)font->metric_num / (float)font->metric_den);
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);

    font->source.lookup = ttfLookup;
    font->source.ascent = toLogical(font, (int)ceilf((float)ascent * font->scale));
    font->source.cell_height = font->source.ascent +
        toLogical(font, (int)ceilf((float)-descent * font->scale));
    font->source.line_height =
        toLogical(font, (int)ceilf((float)(ascent - descent + line_gap) * font->scale));

    // The font's own line spacing is not a layout: the console's system font
    // reports ~27px for a 24px em, which stacks Chinese lines until they touch
    // (the host font reports 36). Keep a floor proportional to the em so any
    // font gets room to breathe.
    {
        int minimum = (int)ceilf(pixel_height * 1.4f);

        if (font->source.line_height < minimum)
            font->source.line_height = minimum;
    }

    font->source.context = font;

    font->loaded = true;

    return true;
}

DglabTtfFont* dglabTtfFontCreate(const void* data, size_t size, float pixel_height,
    int scale_num, int scale_den)
{
    struct DglabTtfFont* font;

    if (g_font_count >= DGLAB_TTF_MAX_SIZES)
        return NULL;

    font = &g_fonts[g_font_count];
    font->metric_num = scale_num > 0 ? scale_num : 1;
    font->metric_den = scale_den > 0 ? scale_den : 1;

    if (!prepare(font, data, size, pixel_height))
        return NULL;

    g_font_count++;

    return font;
}

DglabGlyphSource* dglabTtfFontSource(DglabTtfFont* font)
{
    if (font == NULL || !font->loaded)
        return NULL;

    return &font->source;
}

void dglabTtfFontReset(void)
{
    for (unsigned i = 0; i < g_font_count; i++)
        g_fonts[i].loaded = false;

    g_font_count = 0;
}
