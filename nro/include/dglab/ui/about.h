#pragma once

// The about screen: what this is, where the source lives, and the language row
// (docs/nro-ui.md). Drawn like the rest, so tests/canvas renders it too.

#include <dglab/ipc.h>
#include <dglab/ui/language.h>
#include <dglab/ui/text.h>

typedef struct {
    DglabLanguage preference;        ///< auto / zh-Hans / en, the row's value
    DglabLanguage resolved;          ///< what Auto turned into, shown in brackets
    DglabIpcVersion version;         ///< IPC version of the sysmodule
    const char* github_url;
} DglabAboutState;

void dglabAboutDraw(DglabCanvas* canvas, const DglabFontSet* fonts, const DglabAboutState* state);
