#pragma once

// A glyph source backed by a TrueType/OpenType font in memory, rasterised on
// demand and thresholded to one bit (see docs/nro-ui.md for why not antialiased).
//
// Platform independent: the font bytes come from anywhere - the console passes
// the system shared font, the host tools pass a file - so the same renderer
// draws the screens on both.

#include <dglab/ui/text.h>

#include <stdbool.h>
#include <stddef.h>

/// Prepares the font. `pixel_height` is the em size in pixels (24 matches the
/// layout in docs/nro-ui.md). Returns false when the data is not a usable font.
bool dglabTtfFontInit(const void* data, size_t size, float pixel_height);

/// The source for the font passed to dglabTtfFontInit, or NULL when that failed.
DglabGlyphSource* dglabTtfFontSource(void);
