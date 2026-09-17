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
