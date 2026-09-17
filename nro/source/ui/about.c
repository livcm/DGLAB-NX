#include <dglab/ui/about.h>

#include <dglab/ui/list.h>
#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>

// What this is, which release is running, where the source lives, and the
// language - laid out the way the console lays out a text page: the two prose
// lines are white paragraphs, not grey bulleted notes, and the things that are
// values (the release version, the IPC version, the build, the source, the
// language) are rows under them.
//
// The release version has a row of its own and so does the IPC version, and both
// labels say which number they are: they are different numbers from different
// places (docs/ipc.md, "版本"). The header carries the sysmodule state like every
// other page.
//
// Nothing is focused here: left and right switch the language and up and down
// scroll, and Y switches the colour theme - the same three way shape the
// language row has (follow the console / a fixed value). The bottom bar says so
// for the language, the theme and B, but not for the scroll keys: the scrollbar
// already says the page is taller than the screen, and the hint next to it was
// only repeating that (docs/nro-ui.md).

// The rows the page is made of: two paragraphs plus one row per value, the two
// preferences last. The array is sized once here so both the measurement main.c
// asks for and the draw itself walk the same rows.
#define ABOUT_ROW_COUNT 8

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

// The theme row's value, in the same shape as the language one: Auto says what
// it currently means, because "follow the console" on its own does not say
// whether the screen the user is looking at is the light or the dark palette.
static const char* themeValue(const DglabAboutState* state, char* buffer, size_t size)
{
    const char* name;

    switch (state->theme) {
        case DglabThemeMode_Light: name = dglabString(DglabString_AboutThemeLight); break;
        case DglabThemeMode_Dark: name = dglabString(DglabString_AboutThemeDark); break;
        default: name = dglabString(DglabString_AboutThemeAuto); break;
    }

    if (state->theme == DglabThemeMode_Auto) {
        snprintf(buffer, size, "%s (%s)", name,
            state->theme_system_is_dark ? dglabString(DglabString_AboutThemeDark)
                                        : dglabString(DglabString_AboutThemeLight));
        return buffer;
    }

    snprintf(buffer, size, "%s", name);

    return buffer;
}

// Builds the page's rows into `rows` and returns how many there are. The three
// strings that are formatted rather than looked up live in the caller's buffers,
// because a row only keeps a pointer to its value.
static int buildRows(const DglabAboutState* state, DglabRow* rows, char* ipc_version,
    size_t ipc_size, char* language, size_t language_size, char* theme, size_t theme_size)
{
    const DglabTheme* palette = dglabThemeGet();
    int count = 0;

    snprintf(ipc_version, ipc_size, "%u.%u.%u", (unsigned)state->ipc_version.major,
        (unsigned)state->ipc_version.minor, (unsigned)state->ipc_version.patch);
    languageValue(state, language, language_size);
    themeValue(state, theme, theme_size);

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
        .label = dglabString(DglabString_AboutAppVersion),
        .value = state->app_version ? state->app_version : "",
        .value_color = palette->text,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutIpcVersion),
        .value = ipc_version,
        .value_color = palette->text,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutBuild),
        .value = state->build_id ? state->build_id : "",
        .value_color = palette->text,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutSource),
        .value = state->github_url ? state->github_url : "",
        .value_color = palette->accent,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutLanguage),
        .value = language,
        .value_color = palette->text,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_AboutTheme),
        .value = theme,
        .value_color = palette->text,
    };

    return count;
}

int dglabAboutContentHeight(const DglabFontSet* fonts, const DglabAboutState* state)
{
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[ABOUT_ROW_COUNT];
    DglabRowBox boxes[ABOUT_ROW_COUNT];
    char ipc_version[32];
    char language[96];
    char theme[96];
    int count = buildRows(state, rows, ipc_version, sizeof(ipc_version), language,
        sizeof(language), theme, sizeof(theme));

    return dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        ABOUT_ROW_COUNT);
}

void dglabAboutDraw(DglabCanvas* canvas, const DglabFontSet* fonts, const DglabAboutState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[ABOUT_ROW_COUNT];
    DglabRowBox boxes[ABOUT_ROW_COUNT];
    DglabHint hints[3];
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    DglabListPage page;
    char ipc_version[32];
    char language[96];
    char theme_value[96];
    int count = buildRows(state, rows, ipc_version, sizeof(ipc_version), language,
        sizeof(language), theme_value, sizeof(theme_value));
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height = dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH,
        boxes, ABOUT_ROW_COUNT);
    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, view_height, content_height,
        state->offset);

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_AboutTitle));
    dglabPageHeaderStatus(canvas, fonts->value, state->sysmodule_ok);

    dglabPageClipContent(canvas);

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP - page.offset,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = -1,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, count);

    dglabCanvasClearClip(canvas);

    dglabListPageScrollBar(canvas, &page);

    // The scrollbar above is the whole hint for the up/down keys, so the bottom
    // bar carries only what the page cannot say anywhere else.
    hints[0] = (DglabHint){ DglabButton_Left, DglabButton_Right,
        dglabString(DglabString_ActionLanguage), };
    hints[1] = (DglabHint){ DglabButton_Y, DglabButton_None,
        dglabString(DglabString_ActionTheme), };
    hints[2] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionBack), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 3);
}
