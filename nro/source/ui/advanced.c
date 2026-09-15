#include <dglab/ui/advanced.h>

#include <stdio.h>
#include <string.h>

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
static const uint32_t kBackground = DGLAB_RGBA(0x10, 0x14, 0x18, 0xFF);
static const uint32_t kPanel = DGLAB_RGBA(0x1C, 0x22, 0x30, 0xFF);
static const uint32_t kPanelBorder = DGLAB_RGBA(0x2E, 0x38, 0x4C, 0xFF);
static const uint32_t kText = DGLAB_RGBA(0xE8, 0xEA, 0xF0, 0xFF);
static const uint32_t kMuted = DGLAB_RGBA(0x9A, 0xA4, 0xB8, 0xFF);
static const uint32_t kAccent = DGLAB_RGBA(0x6F, 0xE3, 0x8A, 0xFF);
static const uint32_t kWarn = DGLAB_RGBA(0xFF, 0xC9, 0x4D, 0xFF);
static const uint32_t kSelected = DGLAB_RGBA(0x24, 0x2E, 0x40, 0xFF);

static void drawWrapped(DglabCanvas* canvas, const DglabFont* font, int x, int y, int columns,
    const char* text, uint32_t color)
{
    char line[160];
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

        y += LINE_HEIGHT;
        offset += take;

        while (offset < length && text[offset] == ' ')
            offset++;
    }
}

void dglabAdvancedDraw(DglabCanvas* canvas, const DglabFont* font,
    const DglabAdvancedState* state)
{
    const DglabMotionFeedConfig* config = state->config;
    unsigned selected = state->selected;
    int row_x = MARGIN + 20;
    int value_x = MARGIN + PANEL_WIDTH - 24;
    int row_y = PANEL_TOP + 32;

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);
    dglabCanvasText(canvas, font, MARGIN, 20, 1, "DGLAB-NX   advanced (motion)", kText);

    dglabCanvasFill(canvas, MARGIN, PANEL_TOP, PANEL_WIDTH, PANEL_HEIGHT, kPanel);
    dglabCanvasFrame(canvas, MARGIN, PANEL_TOP, PANEL_WIDTH, PANEL_HEIGHT, 2, kPanelBorder);

    for (unsigned setting = 0; setting < (unsigned)DglabMotionSetting_Count; setting++) {
        int y = row_y + (int)setting * ROW_HEIGHT;
        bool is_selected = (setting == selected);
        char value[32];

        dglabMotionSettingsFormat(config, setting, value, sizeof(value));

        if (is_selected) {
            dglabCanvasFill(canvas, MARGIN + 8, y - 4, PANEL_WIDTH - 16, ROW_HEIGHT - 2,
                kSelected);
            dglabCanvasText(canvas, font, MARGIN + 12, y, 1, ">", kAccent);
        }

        dglabCanvasText(canvas, font, row_x, y, 1, dglabMotionSettingName(setting),
            is_selected ? kText : kMuted);
        dglabCanvasText(canvas, font, value_x - dglabCanvasTextWidth(font, 1, value), y, 1, value,
            is_selected ? kAccent : kText);
    }

    // What the highlighted parameter does, and why it is worth touching.
    drawWrapped(canvas, font, MARGIN + 20,
        row_y + (int)DglabMotionSetting_Count * ROW_HEIGHT + 24, (PANEL_WIDTH - 40) / 16,
        dglabMotionSettingDescription(selected), kMuted);

    dglabCanvasText(canvas, font, MARGIN,
        PANEL_TOP + PANEL_HEIGHT + 24, 1,
        state->saved ? "saved to sdmc:/switch/DGLAB-NX/motion.cfg"
                     : "could not write motion.cfg (settings still apply to this run)",
        state->saved ? kAccent : kWarn);

    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 56, 1,
        "D-pad up/down select    left/right change: one step per press", kText);
    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 32, 1,
        "Y reset to the defaults    + back to the menu", kText);
}
