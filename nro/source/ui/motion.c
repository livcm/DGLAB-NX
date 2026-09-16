#include <dglab/ui/motion.h>

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
#define PANEL_HEIGHT 300
#define LINE_HEIGHT 20
#define PANEL_WIDTH 560

// Same palette as screen.c.

static uint32_t toneColor(u32 tone)
{
    switch (tone) {
        case DglabCmdTone_Warn: return kWarn;
        case DglabCmdTone_Error: return kError;
        default: return kText;
    }
}

static void drawLine(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, const char* label,
    const char* value, uint32_t color)
{
    dglabTextDraw(canvas, text, x, y, label, kMuted);
    dglabTextDraw(canvas, text, x + 12 * 16, y, value, color);
}

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

// One channel's live row: whether that side is connected, how hard it is being
// moved, and what the mapping is sending because of it.
static void drawChannel(DglabCanvas* canvas, DglabGlyphSource* text, int x, int y, const char* which,
    bool connected, bool moving, unsigned level, unsigned frequency, uint32_t color)
{
    char buffer[64];

    if (!connected) {
        snprintf(buffer, sizeof(buffer), "%s", dglabString(DglabString_MotionNotConnected));
    } else if (!moving) {
        snprintf(buffer, sizeof(buffer), "%s   %s 0   %ums",
            dglabString(DglabString_MotionStill), dglabString(DglabString_MotionLevel), frequency);
    } else {
        snprintf(buffer, sizeof(buffer), "%s   %s %u   %ums",
            dglabString(DglabString_MotionMoving), dglabString(DglabString_MotionLevel), level,
            frequency);
    }

    dglabTextDraw(canvas, text, x, y, which, kMuted);
    dglabTextDraw(canvas, text, x + 32, y, buffer, connected ? color : kMuted);
}

void dglabMotionScreenDraw(DglabCanvas* canvas, DglabGlyphSource* text,
    const DglabMotionScreenState* state)
{
    int x = MARGIN + 16;
    int line = text->line_height;
    int y = TITLE_HEIGHT + GAP + 24 + line * 2;
    // Room for the two header lines, five rows and the two line description.
    int panel_height = 20 + line * 10;
    char buffer[512];

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);
    dglabTextDraw(canvas, text, MARGIN, (TITLE_HEIGHT - text->cell_height) / 2,
        dglabString(DglabString_MotionTitle), kText);

    dglabCanvasFill(canvas, MARGIN, TITLE_HEIGHT + GAP, PANEL_WIDTH, panel_height, kPanel);
    dglabCanvasFrame(canvas, MARGIN, TITLE_HEIGHT + GAP, PANEL_WIDTH, panel_height, 2,
        kPanelBorder);
    dglabTextDraw(canvas, text, MARGIN + 16, TITLE_HEIGHT + GAP + 10,
        dglabString(DglabString_MotionChannels), kMuted);

    drawLine(canvas, text, x, y, dglabString(DglabString_MotionLink),
        state->link ? state->link : "-", toneColor(state->link_tone));
    y += line;

    drawChannel(canvas, text, x, y, "A", state->left_connected, state->moving_a, state->level_a,
        state->frequency_a, kAccent);
    y += line;

    drawChannel(canvas, text, x, y, "B", state->right_connected, state->moving_b, state->level_b,
        state->frequency_b, kAccent);
    y += line;

    // The channel strength is the volume this waveform is scaled by, so it sits
    // on its own row.
    snprintf(buffer, sizeof(buffer), "A %u/100   B %u/100", state->channel_strength_a,
        state->channel_strength_b);
    drawLine(canvas, text, x, y, dglabString(DglabString_MotionVolume), buffer, kMuted);
    y += line;

    drawLine(canvas, text, x, y, dglabString(DglabString_MotionLastCmd),
        (state->last_upload && state->last_upload[0]) ? state->last_upload : "-",
        state->last_upload_tone == DglabCmdTone_Error ? kError :
            (state->last_upload_tone == DglabCmdTone_Warn ? kWarn : kText));
    y += line;

    drawWrapped(canvas, text, x, y, PANEL_WIDTH - 48, dglabString(DglabString_MotionDesc), kMuted);

    // One block for the safety notes: this mode drives a device that is attached
    // to a body, so they are not footnotes.
    snprintf(buffer, sizeof(buffer), "%s%s", dglabString(DglabString_MotionSafety),
        state->server_running ? dglabString(DglabString_MotionSleepWarning) : "");

    drawWrapped(canvas, text, MARGIN + 16, TITLE_HEIGHT + GAP + panel_height + 24,
        SCREEN_WIDTH - 2 * MARGIN - 32, buffer, kWarn);

    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - line * 2 - 16,
        dglabString(DglabString_MotionClear), kText);
    dglabTextDraw(canvas, text, MARGIN, SCREEN_HEIGHT - line - 16,
        dglabString(DglabString_MotionBack), kText);
}
