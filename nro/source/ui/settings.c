#include <dglab/ui/settings.h>

#include <stdio.h>
#include <string.h>

// Longest value a line may hold. Both keys are words ("auto", "zh-Hans"), so
// this only ever catches a line that is not a setting at all - and a line like
// that keeps the default instead of being read as garbage.
#define SETTINGS_VALUE_MAX 16

void dglabAppSettingsDefault(DglabAppSettings* settings)
{
    if (settings == NULL)
        return;

    settings->language = DglabLanguage_Auto;
    settings->theme = DglabThemeMode_Auto;
}

void dglabAppSettingsSerialize(const DglabAppSettings* settings, char* out,
    unsigned out_size)
{
    DglabAppSettings defaults;

    if (out == NULL || out_size == 0)
        return;

    if (settings == NULL) {
        dglabAppSettingsDefault(&defaults);
        settings = &defaults;
    }

    snprintf(out, out_size,
        "language=%s\ntheme=%s\n",
        dglabLanguageKey(settings->language),
        dglabThemeModeKey(settings->theme));
}

// One `key=value` line. Anything this build does not know - another key, no
// name at all, a value wider than a word - is left alone, so it keeps whatever
// the defaults (or an earlier line) said.
static void applyLine(const char* line, size_t length, DglabAppSettings* settings)
{
    const char* equal = memchr(line, '=', length);
    size_t key_length;
    size_t value_length;
    char value[SETTINGS_VALUE_MAX];

    if (equal == NULL)
        return;

    key_length = (size_t)(equal - line);
    value_length = length - key_length - 1;

    if (value_length >= sizeof(value))
        return;

    memcpy(value, equal + 1, value_length);
    value[value_length] = '\0';

    // Trailing spaces or a stray CR must not change the meaning.
    while (value_length > 0 &&
           (value[value_length - 1] == ' ' || value[value_length - 1] == '\r'))
        value[--value_length] = '\0';

    if (key_length == 8 && memcmp(line, "language", 8) == 0)
        settings->language = dglabLanguageFromKey(value);
    else if (key_length == 5 && memcmp(line, "theme", 5) == 0)
        settings->theme = dglabThemeModeFromKey(value);
}

void dglabAppSettingsParse(const char* text, DglabAppSettings* settings)
{
    const char* cursor = text;

    if (settings == NULL)
        return;

    dglabAppSettingsDefault(settings);

    if (text == NULL)
        return;

    while (*cursor) {
        const char* line_end = strchr(cursor, '\n');
        size_t length = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

        if (length > 0)
            applyLine(cursor, length, settings);

        if (line_end == NULL)
            break;

        cursor = line_end + 1;
    }
}
