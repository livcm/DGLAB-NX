#include <dglab/ui/about.h>

#include <dglab/ui/list.h>
#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

// What this is, which release is running, where the source lives, and the
// language - laid out the way the console lays out a text page: the two prose
// lines are white paragraphs, not grey bulleted notes, and the things that are
// values (the IPC version, the build, the source, the language) are rows under
// them.
//
// The release version of this NRO goes in the header, the way the other pages
// put their state there; it is the page's own subject rather than one more row.
// The IPC version stays a row, and says so in its label: it is the sysmodule's
// interface version, a different number entirely (docs/ipc.md, "版本").
//
// Nothing is focused here: left and right switch the language, which is what the
// bottom bar says.

static const char* languageValue(const DglabAboutState* state, char* buffer, size_t size)
{
    const char* name;

    switch (state->resolved) {
        case DglabLanguage_ChineseSimplified: name = dglabString(DglabString_AboutLangZh); break;
        default: name = dglabString(DglabString_AboutLangEn); break;
    }

    switch (state->preference) {
        case DglabLanguage_Auto:
            // Say what Auto currently means, so the row is not a mystery.
            snprintf(buffer, size, "%s (%s)", dglabString(DglabString_AboutLangAuto), name);
            break;
        case DglabLanguage_ChineseSimplified:
            snprintf(buffer, size, "%s", dglabString(DglabString_AboutLangZh));
            break;
        default:
            snprintf(buffer, size, "%s", dglabString(DglabString_AboutLangEn));
            break;
    }

    return buffer;
}

void dglabAboutDraw(DglabCanvas* canvas, const DglabFontSet* fonts, const DglabAboutState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[6];
    DglabRowBox boxes[6];
    DglabHint hints[2];
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    char ipc_version[32];
    char language[96];
    int count = 0;
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height;
    int offset;

    snprintf(ipc_version, sizeof(ipc_version), "%u.%u.%u", (unsigned)state->ipc_version.major,
        (unsigned)state->ipc_version.minor, (unsigned)state->ipc_version.patch);
    languageValue(state, language, sizeof(language));

    memset(rows, 0, sizeof(rows));

    rows[count++] = (DglabRow){
        .kind = DglabRow_Paragraph,
        .label = dglabString(DglabString_AboutLine1),
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Paragraph,
        .label = dglabString(DglabString_AboutLine2),
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutIpcVersion),
        .value = ipc_version,
        .value_color = theme->text,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutBuild),
        .value = state->build_id ? state->build_id : "",
        .value_color = theme->text,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutSource),
        .value = state->github_url ? state->github_url : "",
        .value_color = theme->accent,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutLanguage),
        .value = language,
        .value_color = theme->text,
    };

    content_height = dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        (int)(sizeof(boxes) / sizeof(boxes[0])));
    offset = 0;

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_AboutTitle),
        &(DglabTextStyle){ fonts->value, theme->muted },
        state->app_version ? state->app_version : "");

    dglabPageClipContent(canvas);

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP - offset,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = -1,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, count);

    dglabCanvasClearClip(canvas);

    dglabListScrollBar(canvas, DGLAB_PAGE_CONTENT_TOP, view_height, content_height, offset);

    hints[0] = (DglabHint){ DglabButton_Left, DglabButton_Right,
        dglabString(DglabString_ActionLanguage), };
    hints[1] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionBack), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 2);
}
