#pragma once

// Shared by the host tests: reads the .json files the NRO ships, so a screen
// test draws the same text the console does, and a translation that lost a key
// fails the test instead of quietly falling back (docs/nro-ui.md). The path
// comes from the Makefile, which knows where the repository root is.

#include <dglab/ui/language.h>
#include <dglab/ui/strings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DGLAB_TEST_LANG_DIR
#error "DGLAB_TEST_LANG_DIR has to point at the repository's lang/ directory"
#endif

// Reads one language the way the NRO does. Returns false and fills `message`
// when the file is missing, unreadable or less than complete.
static bool dglabTestLoadLanguage(DglabLanguage language, char* message, size_t message_size)
{
    char path[512];
    char* text;
    FILE* file;
    long size;
    DglabStringsReport report;
    DglabStringsLoadResult result;

    snprintf(path, sizeof(path), "%s/%s.json", DGLAB_TEST_LANG_DIR, dglabLanguageKey(language));

    file = fopen(path, "rb");

    if (file == NULL) {
        snprintf(message, message_size, "%s: cannot be opened", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(message, message_size, "%s: cannot be sized", path);
        return false;
    }

    size = ftell(file);

    if (size <= 0) {
        fclose(file);
        snprintf(message, message_size, "%s: empty", path);
        return false;
    }

    rewind(file);
    text = malloc((size_t)size + 1);

    if (text == NULL || fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        fclose(file);
        snprintf(message, message_size, "%s: cannot be read", path);
        return false;
    }

    fclose(file);
    text[size] = '\0';

    result = dglabStringsLoadJson(language, text, (size_t)size, &report);
    free(text);

    if (result == DglabStringsLoad_Failed) {
        snprintf(message, message_size, "%s: line %u: %s", path, report.line,
            report.detail[0] != '\0' ? report.detail : "unusable");
        return false;
    }

    if (report.problem != DglabStringsProblem_None) {
        snprintf(message, message_size, "%s: %s", path, report.detail);
        return false;
    }

    return true;
}

// Loads every language the UI has. Returns false and fills `message` when one
// of them is missing or has something wrong with it.
static bool dglabTestLoadLanguages(char* message, size_t message_size)
{
    static const DglabLanguage kLanguages[] = {
        DglabLanguage_ChineseSimplified,
        DglabLanguage_English,
    };

    dglabStringsReset();

    for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); i++) {
        if (!dglabTestLoadLanguage(kLanguages[i], message, message_size))
            return false;
    }

    return true;
}
