#pragma once

// The about screen: what this is, which release and build is running, where the
// source lives, and the two preference rows - language and colour theme
// (docs/nro-ui.md). Drawn like the rest, so tests/canvas renders it too.
//
// Two of the values are version numbers and they are not the same number: the
// release version of this NRO (VERSION, see dglab/nro/version.h) has a row of its
// own, and so does the sysmodule's interface version from GET_VERSION
// (docs/ipc.md, "版本"). The header carries the sysmodule state like every other
// page, so a version number is never mistaken for it.

#include <dglab/ipc.h>
#include <dglab/ui/language.h>
#include <dglab/ui/list.h> // DGLAB_PARAGRAPH_LINE
#include <dglab/ui/text.h>
#include <dglab/ui/theme.h>

#include <stdbool.h>

/// One press of the page's up/down keys moves it by one paragraph line - the
/// pitch of the text the page is made of - so a step reveals whole lines of the
/// paragraph that did not fit. There is no repeat: the page is at most a few
/// lines taller than the screen.
#define DGLAB_ABOUT_SCROLL_STEP DGLAB_PARAGRAPH_LINE

typedef struct {
    DglabLanguage preference;        ///< auto / zh-Hans / en, the row's value
    DglabLanguage resolved;          ///< what Auto turned into, shown in brackets
    DglabThemeMode theme;            ///< auto / light / dark, the row below it
    bool theme_system_is_dark;       ///< what this theme's Auto means right now
    const char* app_version;         ///< release version of this NRO, from VERSION
    const char* build_id;            ///< the build stamp the binary carries
    DglabIpcVersion ipc_version;     ///< IPC version the sysmodule reported
    const char* github_url;
    bool sysmodule_ok;               ///< the last ping was answered (the header)
    /// First row the page shows, in pixels. main.c keeps it and clamps it
    /// against dglabAboutContentHeight(); the page clamps it again before
    /// drawing, so a stale value cannot scroll past the content.
    int offset;
} DglabAboutState;

void dglabAboutDraw(DglabCanvas* canvas, const DglabFontSet* fonts, const DglabAboutState* state);

/// How tall this page's rows measure in `fonts`. main.c asks for it to clamp the
/// scroll offset it keeps, the way the log sub-page clamps against the log's own
/// height - the row array itself never leaves this file, so the height the keys
/// stop at and the pixels on screen come from one measurement.
int dglabAboutContentHeight(const DglabFontSet* fonts, const DglabAboutState* state);
