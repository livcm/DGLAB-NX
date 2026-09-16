#pragma once

#include <stdbool.h>
#include <stddef.h>

// Reading the language files, and turning what went wrong into text the user
// can act on. The reading lives here because it is the one part that needs the
// SD card; parsing and lookups stay in dglab/ui/strings.h, which runs on a PC as
// well (docs/nro-ui.md).
//
// Every language the UI has is read from <directory>/<code>.json. The NRO needs
// one file to run, not all of them: a language without a file follows the one
// that loaded (dglabStringsLoadJson).

/// Where the NRO reads its language files from: one directory on the SD card,
/// next to its own config/ and logs/ directories. The files are not compiled
/// into the NRO, so this is the whole layout the user has to keep in mind.
#define DGLAB_LANG_DIR "sdmc:/switch/DGLAB-NX/lang"

/// Lines a startup problem is reported in. They also go into the log ring the
/// socket screen shows, where a line is 40 characters wide.
#define DGLAB_LANG_NOTE_MAX 6
#define DGLAB_LANG_NOTE_LEN 40

/// The text the console shows when no language file could be read at all.
#define DGLAB_LANG_ERROR_MAX 1024

typedef struct {
    bool loaded; ///< at least one language file was read
    bool complete; ///< and every language file was there and without a complaint
    unsigned languages; ///< how many files were read
    unsigned notes; ///< how many lines `note` holds
    char note[DGLAB_LANG_NOTE_MAX][DGLAB_LANG_NOTE_LEN];
    char error[DGLAB_LANG_ERROR_MAX]; ///< filled only when `loaded` is false
} DglabLangReport;

/// Reads every language file the UI has out of `directory` and hands the text
/// to the string tables. The NRO passes DGLAB_LANG_DIR; the host tests pass a
/// directory of their own. `report` is always written out in full.
void dglabLangFilesLoad(const char* directory, DglabLangReport* report);
