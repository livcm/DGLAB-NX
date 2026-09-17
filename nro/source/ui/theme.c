#include <dglab/ui/theme.h>

#include <string.h>

const DglabTheme dglabThemeDark = {
    // Measured off the console's own dark theme; docs/nro-ui.md keeps the table
    // and the reasoning, this file only keeps the numbers.
    .background = DGLAB_RGBA(0x2D, 0x2D, 0x2D, 0xFF),
    .surface = DGLAB_RGBA(0x32, 0x32, 0x32, 0xFF),
    .separator = DGLAB_RGBA(0x4D, 0x4D, 0x4D, 0xFF),
    .rule = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF),
    .text = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF),
    .muted = DGLAB_RGBA(0xAA, 0xAA, 0xAA, 0xFF),
    .accent = DGLAB_RGBA(0x00, 0xFF, 0xC8, 0xFF),
    .focus_ring = DGLAB_RGBA(0x66, 0xD5, 0xEE, 0xFF),
    .focus_fill = DGLAB_RGBA(0x1F, 0x23, 0x28, 0xFF),
    .scrollbar = DGLAB_RGBA(0x56, 0x56, 0x56, 0xFF),
    .dialog = DGLAB_RGBA(0x46, 0x46, 0x46, 0xFF),
    .dialog_rule = DGLAB_RGBA(0x67, 0x67, 0x67, 0xFF),
    .dialog_button = DGLAB_RGBA(0x3A, 0x3E, 0x43, 0xFF),
    .warn = DGLAB_RGBA(0xFF, 0xC9, 0x4D, 0xFF),
    .error = DGLAB_RGBA(0xFF, 0x6B, 0x6B, 0xFF),
    .white = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF),
    .black = DGLAB_RGBA(0x00, 0x00, 0x00, 0xFF),
};

const DglabTheme dglabThemeLight = {
    // Measured off the console's own light theme - one 1280x720 screenshot of
    // the system settings (docs/nro-ui.md has the table). The page is light and
    // the rules are dark, which is the dark theme the other way round: the same
    // fields are filled in, so no screen needs to know which one is up.
    //
    // Five of the values could not be measured, because that screenshot holds no
    // error text and no dialog: dialog, dialog_rule, dialog_button, warn and
    // error keep the dark theme's numbers until a light screenshot with those
    // states exists (docs/nro-ui.md says so).
    .background = DGLAB_RGBA(0xEB, 0xEB, 0xEB, 0xFF),
    .surface = DGLAB_RGBA(0xF0, 0xF0, 0xF0, 0xFF),
    .separator = DGLAB_RGBA(0xC9, 0xC9, 0xC9, 0xFF),
    .rule = DGLAB_RGBA(0x2D, 0x2D, 0x2D, 0xFF),
    .text = DGLAB_RGBA(0x2D, 0x2D, 0x2D, 0xFF),
    .muted = DGLAB_RGBA(0x76, 0x76, 0x76, 0xFF),
    .accent = DGLAB_RGBA(0x34, 0x50, 0xF3, 0xFF),
    .focus_ring = DGLAB_RGBA(0x5A, 0xFC, 0xDC, 0xFF),
    .focus_fill = DGLAB_RGBA(0xFD, 0xFD, 0xFD, 0xFF),
    .scrollbar = DGLAB_RGBA(0xC6, 0xC6, 0xC6, 0xFF),
    .dialog = DGLAB_RGBA(0x46, 0x46, 0x46, 0xFF),
    .dialog_rule = DGLAB_RGBA(0x67, 0x67, 0x67, 0xFF),
    .dialog_button = DGLAB_RGBA(0x3A, 0x3E, 0x43, 0xFF),
    .warn = DGLAB_RGBA(0xFF, 0xC9, 0x4D, 0xFF),
    .error = DGLAB_RGBA(0xFF, 0x6B, 0x6B, 0xFF),
    .white = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF),
    .black = DGLAB_RGBA(0x00, 0x00, 0x00, 0xFF),
};

static const DglabTheme* g_active = &dglabThemeDark;

DglabThemeMode dglabThemeModeNext(DglabThemeMode mode)
{
    unsigned next = (unsigned)mode + 1u;

    if (next >= (unsigned)DglabThemeMode_Count)
        next = 0;

    return (DglabThemeMode)next;
}

const char* dglabThemeModeKey(DglabThemeMode mode)
{
    switch (mode) {
        case DglabThemeMode_Light: return "light";
        case DglabThemeMode_Dark: return "dark";
        default: return "auto";
    }
}

DglabThemeMode dglabThemeModeFromKey(const char* key)
{
    if (!key)
        return DglabThemeMode_Auto;

    for (unsigned i = 0; i < (unsigned)DglabThemeMode_Count; i++) {
        if (strcmp(key, dglabThemeModeKey((DglabThemeMode)i)) == 0)
            return (DglabThemeMode)i;
    }

    return DglabThemeMode_Auto;
}

const DglabTheme* dglabThemeResolve(DglabThemeMode mode, bool system_is_dark)
{
    switch (mode) {
        case DglabThemeMode_Light: return &dglabThemeLight;
        case DglabThemeMode_Dark: return &dglabThemeDark;
        case DglabThemeMode_Auto: return system_is_dark ? &dglabThemeDark : &dglabThemeLight;
        default:
            // Not a preference this build knows: dark is what a console that
            // cannot be asked falls back to as well.
            return &dglabThemeDark;
    }
}

const DglabTheme* dglabThemeGet(void)
{
    return g_active;
}

void dglabThemeSet(const DglabTheme* theme)
{
    g_active = theme ? theme : &dglabThemeDark;
}
