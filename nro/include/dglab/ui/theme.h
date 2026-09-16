#pragma once

// Colours live here and nowhere else, so a second theme is a second struct
// rather than a search through the screens (see docs/nro-ui.md). Screens read
// them through dglabThemeGet().

#include <dglab/ui/canvas.h>

typedef struct {
    uint32_t background;
    uint32_t panel;
    uint32_t panel_border;
    uint32_t selected;   ///< row highlight
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
    uint32_t warn;
    uint32_t error;
    uint32_t white;
    uint32_t black;
} DglabTheme;

/// The dark theme every screen was designed against.
extern const DglabTheme dglabThemeDark;

/// The theme the screens draw with. Defaults to dglabThemeDark.
const DglabTheme* dglabThemeGet(void);

/// Switches the active theme; NULL restores the default. (The UI has no picker
/// yet - this exists so adding one, or a light theme, touches nothing else.)
void dglabThemeSet(const DglabTheme* theme);
