#pragma once

// The NRO's own settings file (SD:/switch/DGLAB-NX/config/app.cfg) and what it
// holds: the UI language and the colour theme, both of them preferences rather
// than resolved values (docs/nro-ui.md).
//
// One module owns the file, so a second setting cannot make the first one
// disappear: the two preferences used to be written by the language module
// alone, and appending a line there would have dropped whatever the theme wrote.
// The format is one `key=value` per line, parsed order independently, and every
// key that is missing - or that a hand edit made unreadable - keeps its default.

#include <dglab/ui/language.h>
#include <dglab/ui/theme.h>

typedef struct {
    DglabLanguage language; ///< auto / zh-Hans / en, what the About page shows
    DglabThemeMode theme;   ///< auto (follow the console) / light / dark
} DglabAppSettings;

/// What a missing file means: follow the console for both.
void dglabAppSettingsDefault(DglabAppSettings* settings);

/// Writes the file's contents: two lines today, in this order. Sized for the
/// caller's buffer rather than a fixed string, so adding a key is one line here.
void dglabAppSettingsSerialize(const DglabAppSettings* settings, char* out,
    unsigned out_size);

/// Reads the file's text back. The result always starts from the defaults, so an
/// app.cfg written before a setting existed (one `language=` line and nothing
/// else) upgrades without losing what it did have, and an unknown value is the
/// same as the line not being there at all.
void dglabAppSettingsParse(const char* text, DglabAppSettings* settings);
