#include <dglab/ui/advanced.h>

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
#define ROW_HEIGHT 24
#define PANEL_TOP (TITLE_HEIGHT + GAP)
#define PANEL_WIDTH 720
// Rows, then room for the description of the longest setting (five lines at this
// width) plus a margin, so nothing is ever drawn over the panel border.
#define DESCRIPTION_LINES 5
#define PANEL_HEIGHT \
    (32 + (int)DglabMotionSetting_Count * ROW_HEIGHT + 24 + DESCRIPTION_LINES * 20 + 20)
#define LINE_HEIGHT 20

// Same palette as screen.c.

// Line breaking comes from the text layer, which knows how wide a character is
// and where a break is allowed (Chinese has no spaces to break at).
static void drawWrapped(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, int width,
    const char* value, uint32_t color)
{
    char line[192];

    while (*value) {
        size_t taken = dglabTextWrapLine(text, value, width, line, sizeof(line));

        if (taken == 0)
            break;

        dglabTextDraw(canvas, text, x, y, line, color);

        y += text->line_height;
        value += taken;

        while (*value == ' ')
            value++;
    }
}

// The parameter rows carry their text in the string tables, so the names and
// descriptions translate with everything else.
static const DglabString kNameKeys[DglabMotionSetting_Count] = {
    [DglabMotionSetting_DeadzoneEnter] = DglabString_SetDeadzoneEnter,
    [DglabMotionSetting_DeadzoneExit] = DglabString_SetDeadzoneExit,
    [DglabMotionSetting_GyroRange] = DglabString_SetGyroRange,
    [DglabMotionSetting_AccelRange] = DglabString_SetAccelRange,
    [DglabMotionSetting_GyroWeight] = DglabString_SetGyroWeight,
    [DglabMotionSetting_AccelWeight] = DglabString_SetAccelWeight,
    [DglabMotionSetting_Attack] = DglabString_SetAttack,
    [DglabMotionSetting_Release] = DglabString_SetRelease,
    [DglabMotionSetting_IdleStop] = DglabString_SetIdleStop,
    [DglabMotionSetting_FrequencyFast] = DglabString_SetFrequencyFast,
    [DglabMotionSetting_FrequencyStill] = DglabString_SetFrequencyStill,
    [DglabMotionSetting_StrengthMax] = DglabString_SetStrengthMax,
};

static const DglabString kDescKeys[DglabMotionSetting_Count] = {
    [DglabMotionSetting_DeadzoneEnter] = DglabString_DescDeadzoneEnter,
    [DglabMotionSetting_DeadzoneExit] = DglabString_DescDeadzoneExit,
    [DglabMotionSetting_GyroRange] = DglabString_DescGyroRange,
    [DglabMotionSetting_AccelRange] = DglabString_DescAccelRange,
    [DglabMotionSetting_GyroWeight] = DglabString_DescGyroWeight,
    [DglabMotionSetting_AccelWeight] = DglabString_DescAccelWeight,
    [DglabMotionSetting_Attack] = DglabString_DescAttack,
    [DglabMotionSetting_Release] = DglabString_DescRelease,
    [DglabMotionSetting_IdleStop] = DglabString_DescIdleStop,
    [DglabMotionSetting_FrequencyFast] = DglabString_DescFrequencyFast,
    [DglabMotionSetting_FrequencyStill] = DglabString_DescFrequencyStill,
    [DglabMotionSetting_StrengthMax] = DglabString_DescStrengthMax,
};

void dglabAdvancedDraw(DglabCanvas* canvas, DglabGlyphSource* text,
    const DglabAdvancedState* state)
{
    const DglabMotionFeedConfig* config = state->config;
    unsigned selected = state->selected;
    int line = text->line_height;
    // Twelve rows share the screen with the description and the footer, so the
    // row is a little tighter than a full line.
    int row_height = line - 2;
    int row_x = MARGIN + 28;
    int value_x = MARGIN + PANEL_WIDTH - 28;
    int row_y = PANEL_TOP + 24;
    // Rows on the left, the description of the selected one on the right: the
    // screen is much wider than one column needs, and stacking them did not fit
    // above the footer at this font size.
    int panel_height = 24 + (int)DglabMotionSetting_Count * row_height + 16;
    int side_x = MARGIN + PANEL_WIDTH + GAP;
    int side_width = SCREEN_WIDTH - side_x - MARGIN;

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);
    dglabTextDraw(canvas, text, MARGIN, (TITLE_HEIGHT - text->cell_height) / 2,
        dglabString(DglabString_AdvancedTitle), kText);

    dglabCanvasFill(canvas, MARGIN, PANEL_TOP, PANEL_WIDTH, panel_height, kPanel);
    dglabCanvasFrame(canvas, MARGIN, PANEL_TOP, PANEL_WIDTH, panel_height, 2, kPanelBorder);

    for (unsigned setting = 0; setting < (unsigned)DglabMotionSetting_Count; setting++) {
        int y = row_y + (int)setting * row_height;
        bool is_selected = (setting == selected);
        char value[32];

        dglabMotionSettingsFormat(config, setting, value, sizeof(value));

        if (is_selected) {
            dglabCanvasFill(canvas, MARGIN + 8, y - 4, PANEL_WIDTH - 16, row_height - 2, kSelected);
            dglabTextDraw(canvas, text, MARGIN + 12, y + 4, ">", kAccent);
        }

        dglabTextDraw(canvas, text, row_x, y + 4, dglabString(kNameKeys[setting]),
            is_selected ? kText : kMuted);
        dglabTextDraw(canvas, text, value_x - dglabTextWidth(text, value), y + 4, value,
            is_selected ? kAccent : kText);
    }

    dglabCanvasFill(canvas, side_x, PANEL_TOP, side_width, panel_height, kPanel);
    dglabCanvasFrame(canvas, side_x, PANEL_TOP, side_width, panel_height, 2, kPanelBorder);
    drawWrapped(canvas, text, side_x + 24, PANEL_TOP + 24, side_width - 48,
        dglabString(kDescKeys[selected]), kMuted);

    dglabTextDraw(canvas, text, MARGIN, PANEL_TOP + panel_height + 20,
        state->saved ? dglabString(DglabString_AdvancedSaved)
                     : dglabString(DglabString_AdvancedSaveFailed),
        state->saved ? kAccent : kWarn);

    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - line * 2 - 16,
        dglabString(DglabString_AdvancedSelect), kText);
    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - line - 16,
        dglabString(DglabString_AdvancedReset), kText);
}
