#include <dglab/ui/menu.h>

#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>
#include <stdio.h>
#include <string.h>

// Colours come from the active theme (nro/include/dglab/ui/theme.h); the names
// below are only there to keep the drawing code readable.
#define kBackground (dglabThemeGet()->background)
#define kPanel (dglabThemeGet()->panel)
#define kPanelBorder (dglabThemeGet()->panel_border)
#define kSelected (dglabThemeGet()->selected)
#define kText (dglabThemeGet()->text)
#define kMuted (dglabThemeGet()->muted)
#define kAccent (dglabThemeGet()->accent)
#define kWarn (dglabThemeGet()->warn)
#define kError (dglabThemeGet()->error)
#define kWhite (dglabThemeGet()->white)
#define kBlack (dglabThemeGet()->black)

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define MARGIN 24
#define GAP 16
#define TITLE_HEIGHT 56
#define ROW_HEIGHT 40
#define PANEL_TOP (TITLE_HEIGHT + GAP)
// Tall enough for the rows plus the description of the selected one, and no
// taller: a mostly empty panel looks like a rendering mistake.
#define PANEL_HEIGHT (24 + (int)DglabMenu_ItemCount * ROW_HEIGHT + 40 + 3 * 20 + 20)

// Same palette as screen.c. The two screens are drawn independently, so they
// each carry their own copy rather than sharing a theme header.

// Names and descriptions live in the string tables (nro/source/ui/strings.c), so
// this only maps a menu entry to its keys.
static const DglabString kItemKeys[DglabMenu_ItemCount] = {
    [DglabMenu_ItemSocket] = DglabString_ItemSocket,
    [DglabMenu_ItemMotion] = DglabString_ItemMotion,
    [DglabMenu_ItemAdvanced] = DglabString_ItemAdvanced,
    [DglabMenu_ItemAbout] = DglabString_ItemAbout,
    [DglabMenu_ItemBlePoc] = DglabString_ItemBlePoc,
};

static const DglabString kDescKeys[DglabMenu_ItemCount] = {
    [DglabMenu_ItemSocket] = DglabString_DescSocket,
    [DglabMenu_ItemMotion] = DglabString_DescMotion,
    [DglabMenu_ItemAdvanced] = DglabString_DescAdvanced,
    [DglabMenu_ItemAbout] = DglabString_DescAbout,
    [DglabMenu_ItemBlePoc] = DglabString_DescBlePoc,
};

const char* dglabMenuItemName(unsigned item)
{
    if (item >= (unsigned)DglabMenu_ItemCount)
        return "?";

    return dglabString(kItemKeys[item]);
}

const char* dglabMenuItemDescription(unsigned item)
{
    if (item >= (unsigned)DglabMenu_ItemCount)
        return "";

    return dglabString(kDescKeys[item]);
}

unsigned dglabMenuMove(unsigned selected, int delta)
{
    int count = (int)DglabMenu_ItemCount;
    int value = (int)selected + delta;

    while (value < 0)
        value += count;

    while (value >= count)
        value -= count;

    return (unsigned)value;
}

// Wraps at spaces when there is one (the descriptions have them), the same way
// the socket screen's log panel wraps its URLs.
static void drawWrapped(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, int columns,
    const char* value, uint32_t color)
{
    char line[128];
    size_t length = strlen(value);
    size_t offset = 0;

    if (columns <= 0 || columns >= (int)sizeof(line))
        return;

    while (offset < length) {
        size_t take = length - offset;

        if (take > (size_t)columns) {
            take = (size_t)columns;

            if (offset + take < length && value[offset + take] != ' ') {
                size_t back = take;

                while (back > 0 && value[offset + back] != ' ')
                    back--;

                if (back > (size_t)columns / 3)
                    take = back;
            }
        }

        memcpy(line, value + offset, take);
        line[take] = '\0';

        dglabTextDraw(canvas, text, x, y, line, color);

        y += text->line_height;
        offset += take;

        while (offset < length && value[offset] == ' ')
            offset++;
    }
}

void dglabMenuDraw(DglabCanvas* canvas, DglabGlyphSource* text, const DglabMenuState* state)
{
    unsigned selected = state ? state->selected : 0;
    int panel_width = SCREEN_WIDTH - MARGIN * 2;
    // Rows and the description follow the font, so the same code works for the
    // 16px bitmap font and the 24px system font (docs/nro-ui.md).
    int row_height = text->line_height + 8;
    int row_y = PANEL_TOP + 24;
    int panel_height = 24 + (int)DglabMenu_ItemCount * row_height + 32 + 4 * text->line_height;
    char right[64];

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);

    // Title bar, with the liveness of the sysmodule on the right: every mode
    // depends on it, and finding out only after entering one wastes a trip
    // through the menu.
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);
    dglabTextDraw(canvas, text, MARGIN, (TITLE_HEIGHT - text->cell_height) / 2,
        dglabString(DglabString_MenuTitle), kText);

    snprintf(right, sizeof(right), "%s",
        (state && state->sysmodule_ok) ? dglabString(DglabString_SysmoduleOk)
                                       : dglabString(DglabString_SysmoduleDown));
    dglabTextDraw(canvas, text, SCREEN_WIDTH - MARGIN - dglabTextWidth(text, right),
        (TITLE_HEIGHT - text->cell_height) / 2, right,
        (state && state->sysmodule_ok) ? kAccent : kError);

    dglabCanvasFill(canvas, MARGIN, PANEL_TOP, panel_width, panel_height, kPanel);
    dglabCanvasFrame(canvas, MARGIN, PANEL_TOP, panel_width, panel_height, 2, kPanelBorder);

    for (unsigned item = 0; item < (unsigned)DglabMenu_ItemCount; item++) {
        int y = row_y + (int)item * row_height;
        bool is_selected = (item == selected);

        if (is_selected) {
            dglabCanvasFill(canvas, MARGIN + 8, y - 6, panel_width - 16, row_height - 2, kSelected);
            dglabTextDraw(canvas, text, MARGIN + 20, y + 4, ">", kAccent);
        }

        dglabTextDraw(canvas, text, MARGIN + 56, y + 4, dglabMenuItemName(item),
            is_selected ? kText : kMuted);
    }

    // What the highlighted mode actually does.
    drawWrapped(canvas, text, MARGIN + 24,
        row_y + (int)DglabMenu_ItemCount * row_height + 28, (panel_width - 48) / 14,
        dglabMenuItemDescription(selected), kMuted);

    // Two footer lines placed from the bottom, using the font's own line height:
    // the fixed 56/24 offsets were sized for the 16px bitmap font and clip the
    // 24px system font.
    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - text->line_height * 2 - 16,
        dglabString(DglabString_MenuSelect), kText);
    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - text->line_height - 16,
        dglabString(DglabString_MenuStart), kText);
}
