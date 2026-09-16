#include <dglab/ui/language.h>

#include <stdio.h>
#include <string.h>

DglabLanguage dglabLanguageResolve(DglabLanguage preference, bool system_is_chinese)
{
    if (preference == DglabLanguage_Auto)
        return system_is_chinese ? DglabLanguage_ChineseSimplified : DglabLanguage_English;

    if (preference < 0 || preference >= DglabLanguage_Count)
        return DglabLanguage_English;

    return preference;
}

DglabLanguage dglabLanguageNext(DglabLanguage preference)
{
    unsigned next = (unsigned)preference + 1u;

    if (next >= (unsigned)DglabLanguage_Count)
        next = 0;

    return (DglabLanguage)next;
}

const char* dglabLanguageKey(DglabLanguage preference)
{
    switch (preference) {
        case DglabLanguage_ChineseSimplified: return "zh-Hans";
        case DglabLanguage_English: return "en";
        default: return "auto";
    }
}

DglabLanguage dglabLanguageFromKey(const char* key)
{
    if (!key)
        return DglabLanguage_Auto;

    for (unsigned i = 0; i < (unsigned)DglabLanguage_Count; i++) {
        if (strcmp(key, dglabLanguageKey((DglabLanguage)i)) == 0)
            return (DglabLanguage)i;
    }

    return DglabLanguage_Auto;
}

void dglabLanguageSerialize(DglabLanguage preference, char* out, unsigned out_size)
{
    if (!out || out_size == 0)
        return;

    snprintf(out, out_size, "language=%s\n", dglabLanguageKey(preference));
}

DglabLanguage dglabLanguageParse(const char* text)
{
    const char* cursor = text;

    if (!text)
        return DglabLanguage_Auto;

    while (*cursor) {
        const char* line_end = strchr(cursor, '\n');
        size_t length = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

        if (length > 0 && strncmp(cursor, "language=", 9) == 0) {
            char value[16];
            size_t value_length = length - 9;

            if (value_length >= sizeof(value))
                value_length = sizeof(value) - 1;

            memcpy(value, cursor + 9, value_length);
            value[value_length] = '\0';

            // Trailing spaces or a stray CR must not change the meaning.
            while (value_length > 0 &&
                   (value[value_length - 1] == ' ' || value[value_length - 1] == '\r'))
                value[--value_length] = '\0';

            return dglabLanguageFromKey(value);
        }

        if (!line_end)
            break;

        cursor = line_end + 1;
    }

    return DglabLanguage_Auto;
}
