#include <dglab/ui/about.h>

#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define MARGIN 24
#define GAP 16
#define TITLE_HEIGHT 56
#define PANEL_WIDTH 900

#define kBackground (dglabThemeGet()->background)
#define kPanel (dglabThemeGet()->panel)
#define kPanelBorder (dglabThemeGet()->panel_border)
#define kText (dglabThemeGet()->text)
#define kMuted (dglabThemeGet()->muted)
#define kAccent (dglabThemeGet()->accent)

// Draws the wrapped text and advances *y past it, so the caller does not have to
// guess how many lines the wrap produced (it did guess, and overlapped).
static void drawWrapped(DglabCanvas* canvas, DglabGlyphSource* text, int x, int* y, int columns,
    const char* value, uint32_t color)
{
    char line[192];
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

        dglabTextDraw(canvas, text, x, *y, line, color);

        *y += text->line_height;
        offset += take;

        while (offset < length && value[offset] == ' ')
            offset++;
    }
}

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

void dglabAboutDraw(DglabCanvas* canvas, DglabGlyphSource* text, const DglabAboutState* state)
{
    int line = text->line_height;
    int x = MARGIN + 24;
    int y = TITLE_HEIGHT + GAP + 32;
    int panel_height = line * 8 + 40;
    char buffer[96];

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);
    dglabTextDraw(canvas, text, MARGIN, 28, dglabString(DglabString_AboutTitle), kText);

    dglabCanvasFill(canvas, MARGIN, TITLE_HEIGHT + GAP, PANEL_WIDTH, panel_height, kPanel);
    dglabCanvasFrame(canvas, MARGIN, TITLE_HEIGHT + GAP, PANEL_WIDTH, panel_height, 2,
        kPanelBorder);

    drawWrapped(canvas, text, x, &y, (PANEL_WIDTH - 48) / 13, dglabString(DglabString_AboutLine1),
        kText);
    y += line / 2;
    drawWrapped(canvas, text, x, &y, (PANEL_WIDTH - 48) / 13, dglabString(DglabString_AboutLine2),
        kMuted);
    y += line;

    snprintf(buffer, sizeof(buffer), "IPC %u.%u.%u", (unsigned)state->version.major,
        (unsigned)state->version.minor, (unsigned)state->version.patch);
    dglabTextDraw(canvas, text, x, y, buffer, kMuted);
    y += line * 2;

    dglabTextDraw(canvas, text, x, y, dglabString(DglabString_AboutSource), kMuted);
    y += line;
    dglabTextDraw(canvas, text, x, y, state->github_url, kAccent);
    y += line * 2;

    dglabTextDraw(canvas, text, x, y, dglabString(DglabString_AboutLanguage), kMuted);
    dglabTextDraw(canvas, text, x + 16 * 13, y, languageValue(state, buffer, sizeof(buffer)), kText);

    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - 44,
        dglabString(DglabString_AboutFooter), kText);
}
