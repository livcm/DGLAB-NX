#pragma once

// Which language the UI draws in. Two packs exist (Simplified Chinese and
// English) but nothing here assumes that: the preference is an enum and the
// tables are indexed by it, so a third one is a third column (docs/nro-ui.md).
//
// How the preference is stored is not this module's business: app.cfg belongs to
// dglab/ui/settings.h, which keeps the language and the theme lines together.

#include <stdbool.h>

typedef enum {
    DglabLanguage_Auto = 0, ///< follow the console
    DglabLanguage_ChineseSimplified,
    DglabLanguage_English,
    DglabLanguage_Count,
} DglabLanguage;

/// Turns a preference into the language actually used: Auto asks the console.
DglabLanguage dglabLanguageResolve(DglabLanguage preference, bool system_is_chinese);

/// Menu order for the language row: Auto -> Chinese -> English -> Auto.
DglabLanguage dglabLanguageNext(DglabLanguage preference);

/// Text of the preference for the settings row ("auto" / "zh-Hans" / "en").
const char* dglabLanguageKey(DglabLanguage preference);

/// Reads a preference back; anything unknown (including an empty string) means
/// Auto, so a hand edited file cannot leave the UI in an undefined state.
DglabLanguage dglabLanguageFromKey(const char* key);
