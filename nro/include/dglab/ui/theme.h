#pragma once

// Colours live here and nowhere else, so a second theme is a second struct
// rather than a search through the screens (see docs/nro-ui.md). Screens read
// them through dglabThemeGet().
//
// The values are the console's own dark theme, measured off screenshots of the
// system settings (docs/nro-ui.md has the table): the surface is a flat grey,
// there is no border to speak of, and a focused row is a dark fill inside a
// light ring rather than a highlighted bar.

#include <dglab/ui/canvas.h>

typedef struct {
    uint32_t background;   ///< #2D2D2D, the page
    uint32_t surface;      ///< #323232, a raised block on the page
    uint32_t separator;    ///< #4D4D4D, the rule between list rows
    uint32_t rule;         ///< #FFFFFF, the rule under the title bar
    uint32_t text;         ///< #FFFFFF
    uint32_t muted;        ///< #AAAAAA, descriptions and dimmed values
    uint32_t accent;       ///< #00FFC8, values, the menu's selection bar
    uint32_t focus_ring;   ///< #66D5EE, the ring around the focused row
    uint32_t focus_fill;   ///< #1F2328, the fill inside that ring
    uint32_t scrollbar;    ///< #565656
    uint32_t dialog;       ///< #464646, the modal panel
    uint32_t dialog_rule;  ///< #676767, the rule above its buttons
    uint32_t dialog_button;///< #3A3E43, the focused button's fill
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
