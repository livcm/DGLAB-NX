#pragma once

// The console side of text: libnx's `pl` service hands over the system shared
// font, which is the only CJK font a homebrew NRO can reach without shipping one
// (see docs/nro-ui.md).

#include <dglab/ui/text.h>
#include <dglab/ui/text_ttf.h>

#include <stdbool.h>

/// Loads the shared font for the language and prepares every size in
/// DglabFontSet (title, body, value, note and the button icons), rasterised at
/// the display's own scale.
/// `chinese` picks the Simplified Chinese face; otherwise the standard one.
/// Returns NULL when the font could not be loaded, in which case the caller
/// falls back to the built in bitmap font (ASCII only).
const DglabFontSet* dglabFontOpen(bool chinese);

void dglabFontClose(void);

/// Whether the console's system language is Simplified Chinese. Starts as true
/// when the system cannot be asked.
bool dglabFontSystemIsChinese(void);
