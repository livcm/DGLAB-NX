#include <dglab/platform/langfiles.h>

#include <dglab/ui/language.h>
#include <dglab/ui/strings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Longest path this file builds, and the largest file it accepts as a language
// file. The shipped ones are about 8 KiB.
#define PATH_LEN 192
#define FILE_MAX (64u * 1024u)

typedef enum {
    Read_Ok = 0,
    Read_Missing,
    Read_Unreadable,
    Read_TooLarge,
} ReadResult;

// ---------------------------------------------------------------------------
// Reading one file
// ---------------------------------------------------------------------------

// Grows the buffer while reading instead of asking for the size first: the
// language files are small, and one fewer thing has to be right on the target
// filesystem.
static ReadResult readFile(const char* path, char** out_text, size_t* out_size)
{
    FILE* file = fopen(path, "rb");
    size_t capacity = 4096;
    size_t used = 0;
    char* buffer;

    *out_text = NULL;
    *out_size = 0;

    if (file == NULL)
        return Read_Missing;

    buffer = malloc(capacity + 1);

    if (buffer == NULL) {
        fclose(file);
        return Read_Unreadable;
    }

    for (;;) {
        size_t got;

        if (used == capacity) {
            char* grown;

            if (capacity >= FILE_MAX) {
                free(buffer);
                fclose(file);
                return Read_TooLarge;
            }

            capacity *= 2;

            if (capacity > FILE_MAX)
                capacity = FILE_MAX;

            grown = realloc(buffer, capacity + 1);

            if (grown == NULL) {
                free(buffer);
                fclose(file);
                return Read_Unreadable;
            }

            buffer = grown;
        }

        got = fread(buffer + used, 1, capacity - used, file);

        if (got == 0) {
            if (ferror(file)) {
                free(buffer);
                fclose(file);
                return Read_Unreadable;
            }

            break;
        }

        used += got;
    }

    fclose(file);
    buffer[used] = '\0';
    *out_text = buffer;
    *out_size = used;

    return used > 0 ? Read_Ok : Read_Unreadable;
}

// ---------------------------------------------------------------------------
// What to say about a file
// ---------------------------------------------------------------------------

static const char* problemText(const DglabStringsReport* report, char* buffer, size_t buffer_size)
{
    switch (report->problem) {
        case DglabStringsProblem_MissingKeys:
            snprintf(buffer, buffer_size, "%u keys missing", report->missing);
            break;
        case DglabStringsProblem_UnknownKeys:
            snprintf(buffer, buffer_size, "%u unknown keys, first '%s'", report->unknown,
                report->detail);
            break;
        case DglabStringsProblem_DuplicateKey:
            snprintf(buffer, buffer_size, "duplicate key '%s'", report->detail);
            break;
        case DglabStringsProblem_Language:
            snprintf(buffer, buffer_size, "it says it is '%s'", report->language);
            break;
        case DglabStringsProblem_Syntax:
        case DglabStringsProblem_Structure:
            snprintf(buffer, buffer_size, "line %u: %s", report->line, report->detail);
            break;
        case DglabStringsProblem_OutOfMemory:
            snprintf(buffer, buffer_size, "out of memory");
            break;
        case DglabStringsProblem_Unsupported:
            snprintf(buffer, buffer_size, "no such language");
            break;
        default:
            snprintf(buffer, buffer_size, "unusable");
            break;
    }

    return buffer;
}

static void addNote(DglabLangReport* report, const char* code, const char* what)
{
    if (report->notes >= DGLAB_LANG_NOTE_MAX)
        return;

    // One line of the log panel is 40 characters, so a long reason is cut
    // rather than pushed to a second line.
    snprintf(report->note[report->notes], DGLAB_LANG_NOTE_LEN, "lang: %s.json %s", code, what);
    report->notes++;
}

static void appendText(char* out, size_t out_size, size_t* used, const char* text)
{
    size_t length = strlen(text);

    if (*used + length + 1 > out_size)
        length = out_size > *used + 1 ? out_size - *used - 1 : 0;

    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
}

// ---------------------------------------------------------------------------
// The whole job
// ---------------------------------------------------------------------------

void dglabLangFilesLoad(const char* directory, DglabLangReport* report)
{
    char reason[DglabLanguage_Count][192];
    bool loaded_language[DglabLanguage_Count];
    unsigned wanted = 0;
    bool complete = true;
    size_t filled = 0;

    memset(report, 0, sizeof(*report));
    memset(loaded_language, 0, sizeof(loaded_language));

    for (int i = 0; i < DglabLanguage_Count; i++) {
        DglabLanguage language = (DglabLanguage)i;
        char path[PATH_LEN];
        char text_reason[192];
        char* text = NULL;
        size_t size = 0;
        DglabStringsReport strings;
        DglabStringsLoadResult result;
        ReadResult read;
        const char* code;

        if (language == DglabLanguage_Auto)
            continue;

        wanted++;
        code = dglabLanguageKey(language);
        snprintf(reason[language], sizeof(reason[language]), "not found");
        snprintf(path, sizeof(path), "%s/%s.json", directory, code);

        read = readFile(path, &text, &size);

        if (read == Read_Ok) {
            result = dglabStringsLoadJson(language, text, size, &strings);
            free(text);

            if (result != DglabStringsLoad_Failed) {
                loaded_language[language] = true;
                report->languages++;

                if (strings.problem != DglabStringsProblem_None) {
                    complete = false;
                    addNote(report, code, problemText(&strings, text_reason, sizeof(text_reason)));
                }
            } else {
                snprintf(reason[language], sizeof(reason[language]), "%s",
                    problemText(&strings, text_reason, sizeof(text_reason)));
            }
        } else if (read != Read_Missing) {
            snprintf(reason[language], sizeof(reason[language]), "%s",
                read == Read_TooLarge ? "too large to be a language file" : "unreadable");
        }
    }

    for (int i = 0; i < DglabLanguage_Count; i++) {
        DglabLanguage language = (DglabLanguage)i;

        if (language == DglabLanguage_Auto || loaded_language[language])
            continue;

        complete = false;
        addNote(report, dglabLanguageKey(language), reason[language]);
    }

    report->loaded = report->languages > 0;
    report->complete = complete && report->languages == wanted;

    if (report->loaded)
        return;

    // Nothing could be read: this is the text the console shows before the UI
    // is allowed to start.
    appendText(report->error, sizeof(report->error), &filled,
        "no language file could be read.\n\nlooked in:\n\n  ");
    appendText(report->error, sizeof(report->error), &filled, directory);
    appendText(report->error, sizeof(report->error), &filled, "\n\n");

    for (int i = 0; i < DglabLanguage_Count; i++) {
        DglabLanguage language = (DglabLanguage)i;

        if (language == DglabLanguage_Auto)
            continue;

        appendText(report->error, sizeof(report->error), &filled, dglabLanguageKey(language));
        appendText(report->error, sizeof(report->error), &filled, ".json: ");
        appendText(report->error, sizeof(report->error), &filled, reason[language]);
        appendText(report->error, sizeof(report->error), &filled, "\n");
    }

    appendText(report->error, sizeof(report->error), &filled,
        "\nCopy the lang/ directory that comes with the release here, then\n"
        "start this homebrew again.\n");
}
