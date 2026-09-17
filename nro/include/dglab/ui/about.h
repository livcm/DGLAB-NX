#pragma once

// The about screen: what this is, which release and build is running, where the
// source lives, and the language row (docs/nro-ui.md). Drawn like the rest, so
// tests/canvas renders it too.
//
// Two of the values are version numbers and they are not the same number: the
// header carries the release version of this NRO (VERSION, see
// dglab/nro/version.h), the IPC row carries the sysmodule's interface version
// from GET_VERSION (docs/ipc.md, "版本").

#include <dglab/ipc.h>
#include <dglab/ui/language.h>
#include <dglab/ui/text.h>

typedef struct {
    DglabLanguage preference;        ///< auto / zh-Hans / en, the row's value
    DglabLanguage resolved;          ///< what Auto turned into, shown in brackets
    const char* app_version;         ///< release version of this NRO, from VERSION
    const char* build_id;            ///< the build stamp the binary carries
    DglabIpcVersion ipc_version;     ///< IPC version the sysmodule reported
    const char* github_url;
} DglabAboutState;

void dglabAboutDraw(DglabCanvas* canvas, const DglabFontSet* fonts, const DglabAboutState* state);
