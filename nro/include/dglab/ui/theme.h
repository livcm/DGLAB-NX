#pragma once

// Colours live here and nowhere else, so a second theme is a second struct
// rather than a search through the screens (see docs/nro-ui.md). Screens read
// them through dglabThemeGet(), and never ask which one is up.
//
// Both palettes are the console's own, measured off screenshots of the system
// settings - the dark one and the light one (docs/nro-ui.md has both tables):
// the surface is a flat grey, there is no border to speak of, and a focused row
// is a fill inside a coloured ring rather than a highlighted bar.
//
// Which palette is up is the user's three way preference (follow the console,
// light, dark); the screens only ever see the result of it.

#include <dglab/ui/canvas.h>

#include <stdbool.h>

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

/// The light theme, measured off the console's own light mode. Same fields, so
/// a screen that draws with one draws with the other.
extern const DglabTheme dglabThemeLight;

/// Which palette the user asked for. Auto is the default and asks the console.
typedef enum {
    DglabThemeMode_Auto = 0, ///< follow the console
    DglabThemeMode_Light,
    DglabThemeMode_Dark,
    DglabThemeMode_Count,
} DglabThemeMode;

/// Menu order for the theme row: Auto -> Light -> Dark -> Auto.
DglabThemeMode dglabThemeModeNext(DglabThemeMode mode);

/// Text of the preference for the settings row ("auto" / "light" / "dark").
const char* dglabThemeModeKey(DglabThemeMode mode);

/// Reads a preference back; anything unknown (including an empty string) means
/// Auto, so a hand edited file cannot leave the UI in an undefined state.
DglabThemeMode dglabThemeModeFromKey(const char* key);

/// Turns a preference into the palette to draw with: Auto is whatever the
/// console says (light when it is not dark), and a preference that is not one
/// of the three is dark - the fallback the console cannot answer either.
const DglabTheme* dglabThemeResolve(DglabThemeMode mode, bool system_is_dark);

/// The theme the screens draw with. Defaults to dglabThemeDark.
const DglabTheme* dglabThemeGet(void);

/// Switches the active theme; NULL restores the default. The screens never call
/// this - main.c resolves the preference to a palette once at startup, on a
/// rebuilt display, and when the About page changes the preference.
void dglabThemeSet(const DglabTheme* theme);
