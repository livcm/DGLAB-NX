#include <dglab/ui/menu.h>

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

const char* dglabMenuItemName(unsigned item)
{
    switch (item) {
        case DglabMenu_ItemSocket: return "Socket test";
        case DglabMenu_ItemMotion: return "Motion (Joy-Con)";
        case DglabMenu_ItemAdvanced: return "Advanced (motion)";
        case DglabMenu_ItemBlePoc: return "BLE PoC console";
        default: return "?";
    }
}

const char* dglabMenuItemDescription(unsigned item)
{
    switch (item) {
        case DglabMenu_ItemSocket:
            return "Start the socket server, show the QR code the DG-LAB app scans, and test both "
                   "channels by hand. The server stops itself 55 s after the last app leaves.";
        case DglabMenu_ItemMotion:
            return "Drive the waveform with the Joy-Cons: the more one moves, the stronger and "
                   "denser its channel gets. Left Joy-Con is channel A, right is B. Start the "
                   "socket server in Socket test first.";
        case DglabMenu_ItemAdvanced:
            return "Every motion parameter on one page - dead zone, sensitivity, envelope, "
                   "frequency and the waveform strength - edited one step at a time and saved "
                   "to the SD card, so a tuning session survives a restart.";
        case DglabMenu_ItemBlePoc:
            return "Console view from the abandoned host side BLE experiments. Kept because it is "
                   "the rendering path that is known to work on real hardware.";
        default:
            return "";
    }
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
static void drawWrapped(DglabCanvas* canvas, const DglabFont* font, int x, int y, int columns,
    const char* text, uint32_t color)
{
    char line[128];
    size_t length = strlen(text);
    size_t offset = 0;

    if (columns <= 0 || columns >= (int)sizeof(line))
        return;

    while (offset < length) {
        size_t take = length - offset;

        if (take > (size_t)columns) {
            take = (size_t)columns;

            if (offset + take < length && text[offset + take] != ' ') {
                size_t back = take;

                while (back > 0 && text[offset + back] != ' ')
                    back--;

                if (back > (size_t)columns / 3)
                    take = back;
            }
        }

        memcpy(line, text + offset, take);
        line[take] = '\0';

        dglabCanvasText(canvas, font, x, y, 1, line, color);

        y += 20;
        offset += take;

        while (offset < length && text[offset] == ' ')
            offset++;
    }
}

void dglabMenuDraw(DglabCanvas* canvas, const DglabFont* font, const DglabMenuState* state)
{
    unsigned selected = state ? state->selected : 0;
    int panel_width = SCREEN_WIDTH - MARGIN * 2;
    int row_y = PANEL_TOP + 24;
    char right[48];

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);

    // Title bar, with the liveness of the sysmodule on the right: every mode
    // depends on it, and finding out only after entering one wastes a trip
    // through the menu.
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);
    dglabCanvasText(canvas, font, MARGIN, 20, 1, "DGLAB-NX   modes", kText);

    snprintf(right, sizeof(right), "%s",
        (state && state->sysmodule_ok) ? "sysmodule ok" : "sysmodule not answering");
    dglabCanvasText(canvas, font, SCREEN_WIDTH - MARGIN - dglabCanvasTextWidth(font, 1, right), 20,
        1, right, (state && state->sysmodule_ok) ? kAccent : kError);

    dglabCanvasFill(canvas, MARGIN, PANEL_TOP, panel_width, PANEL_HEIGHT, kPanel);
    dglabCanvasFrame(canvas, MARGIN, PANEL_TOP, panel_width, PANEL_HEIGHT, 2, kPanelBorder);

    for (unsigned item = 0; item < (unsigned)DglabMenu_ItemCount; item++) {
        int y = row_y + (int)item * ROW_HEIGHT;
        bool is_selected = (item == selected);

        if (is_selected) {
            dglabCanvasFill(canvas, MARGIN + 8, y - 8, panel_width - 16, ROW_HEIGHT - 4,
                kSelected);
            dglabCanvasText(canvas, font, MARGIN + 20, y, 1, ">", kAccent);
        }

        dglabCanvasText(canvas, font, MARGIN + 48, y, 1, dglabMenuItemName(item),
            is_selected ? kText : kMuted);
    }

    // What the highlighted mode actually does.
    drawWrapped(canvas, font, MARGIN + 20, row_y + (int)DglabMenu_ItemCount * ROW_HEIGHT + 40,
        (panel_width - 40) / 16, dglabMenuItemDescription(selected), kMuted);

    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 56, 1,
        "D-pad up/down select", kText);
    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 32, 1,
        "A start mode    + exit", kText);
}
