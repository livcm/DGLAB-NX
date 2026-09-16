#pragma once

// Glyph sources backed by a TrueType/OpenType font in memory, rasterised on
// demand with the coverage stb_truetype hands back (see docs/nro-ui.md for why
// the one bit version looked wrong on hardware).
//
// Platform independent: the font bytes come from anywhere - the console passes
// the system shared font, the host tools pass a file - so the same renderer
// draws the screens on both.
//
// One size is one object with its own glyph cache. The console's UI draws a 28px
// title, 24px rows, 22px values, a 20px button letter and 18px notes on the same
// screen, and a single global font could only ever hold one of those, which is
// what kept the layout on one size until now.

#include <dglab/ui/text.h>

#include <stdbool.h>
#include <stddef.h>

/// One prepared size of a face.
typedef struct DglabTtfFont DglabTtfFont;

/// How many sizes can be prepared at once: the five in text.h. Preparing a sixth
/// fails rather than silently reusing a cache.
#define DGLAB_TTF_MAX_SIZES 5

/// Prepares one size. `pixel_height` is the em size in *logical* pixels (24 for a
/// row, see text.h); `scale_num`/`scale_den` are the display's physical pixels
/// per logical pixel, so a docked console rasterises 42px bitmaps for the 28px
/// title while every metric the layout reads stays logical. Returns NULL when the
/// data is not a usable font or no slot is left.
DglabTtfFont* dglabTtfFontCreate(const void* data, size_t size, float pixel_height,
    int scale_num, int scale_den);

/// The glyph source for a prepared size, for the text layer.
DglabGlyphSource* dglabTtfFontSource(DglabTtfFont* font);

/// Forgets every prepared size. The console calls it when it closes the shared
/// font: the next face is rasterised into the same four caches.
void dglabTtfFontReset(void);
