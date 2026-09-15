#include <dglab/ui/motion.h>

#include <stdio.h>
#include <string.h>

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define MARGIN 24
#define GAP 16
#define TITLE_HEIGHT 56
#define PANEL_HEIGHT 300
#define LINE_HEIGHT 20
#define PANEL_WIDTH 560

// Same palette as screen.c.
static const uint32_t kBackground = DGLAB_RGBA(0x10, 0x14, 0x18, 0xFF);
static const uint32_t kPanel = DGLAB_RGBA(0x1C, 0x22, 0x30, 0xFF);
static const uint32_t kPanelBorder = DGLAB_RGBA(0x2E, 0x38, 0x4C, 0xFF);
static const uint32_t kText = DGLAB_RGBA(0xE8, 0xEA, 0xF0, 0xFF);
static const uint32_t kMuted = DGLAB_RGBA(0x9A, 0xA4, 0xB8, 0xFF);
static const uint32_t kAccent = DGLAB_RGBA(0x6F, 0xE3, 0x8A, 0xFF);
static const uint32_t kWarn = DGLAB_RGBA(0xFF, 0xC9, 0x4D, 0xFF);
static const uint32_t kError = DGLAB_RGBA(0xFF, 0x6B, 0x6B, 0xFF);

static uint32_t toneColor(u32 tone)
{
    switch (tone) {
        case DglabCmdTone_Warn: return kWarn;
        case DglabCmdTone_Error: return kError;
        default: return kText;
    }
}

static void drawLine(DglabCanvas* canvas, const DglabFont* font, int x, int y, const char* label,
    const char* value, uint32_t color)
{
    dglabCanvasText(canvas, font, x, y, 1, label, kMuted);
    dglabCanvasText(canvas, font, x + 12 * 16, y, 1, value, color);
}

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

        y += LINE_HEIGHT;
        offset += take;

        while (offset < length && text[offset] == ' ')
            offset++;
    }
}

// One channel's live row: whether that side is connected, how hard it is being
// moved, and what the mapping is sending because of it.
static void drawChannel(DglabCanvas* canvas, const DglabFont* font, int x, int y, const char* which,
    bool connected, bool moving, unsigned level, unsigned frequency, uint32_t color)
{
    char buffer[64];

    if (!connected)
        snprintf(buffer, sizeof(buffer), "not connected");
    else if (!moving)
        snprintf(buffer, sizeof(buffer), "still      level 0    %ums", frequency);
    else
        snprintf(buffer, sizeof(buffer), "moving     level %u    %ums", level, frequency);

    dglabCanvasText(canvas, font, x, y, 1, which, kMuted);
    dglabCanvasText(canvas, font, x + 32, y, 1, buffer, connected ? color : kMuted);
}

void dglabMotionScreenDraw(DglabCanvas* canvas, const DglabFont* font,
    const DglabMotionScreenState* state)
{
    int x = MARGIN + 16;
    int y = TITLE_HEIGHT + GAP + 12 + LINE_HEIGHT * 2;
    char buffer[512];

    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, kBackground);
    dglabCanvasFill(canvas, 0, 0, SCREEN_WIDTH, TITLE_HEIGHT, kPanel);
    dglabCanvasText(canvas, font, MARGIN, 20, 1, "DGLAB-NX   motion (Joy-Con)", kText);

    dglabCanvasFill(canvas, MARGIN, TITLE_HEIGHT + GAP, PANEL_WIDTH, PANEL_HEIGHT, kPanel);
    dglabCanvasFrame(canvas, MARGIN, TITLE_HEIGHT + GAP, PANEL_WIDTH, PANEL_HEIGHT, 2,
        kPanelBorder);
    dglabCanvasText(canvas, font, MARGIN + 12, TITLE_HEIGHT + GAP + 8, 1, "channels", kMuted);

    drawLine(canvas, font, x, y, "link", state->link ? state->link : "-", toneColor(state->link_tone));
    y += LINE_HEIGHT;

    drawChannel(canvas, font, x, y, "A", state->left_connected, state->moving_a, state->level_a,
        state->frequency_a, kAccent);
    y += LINE_HEIGHT;

    drawChannel(canvas, font, x, y, "B", state->right_connected, state->moving_b, state->level_b,
        state->frequency_b, kAccent);
    y += LINE_HEIGHT;

    // The channel strength is the volume this waveform is scaled by, so it sits
    // on its own row rather than being crammed into the moving rows.
    snprintf(buffer, sizeof(buffer), "A %u/100  B %u/100", state->channel_strength_a,
        state->channel_strength_b);
    drawLine(canvas, font, x, y, "volume", buffer, kMuted);
    y += LINE_HEIGHT;

    drawLine(canvas, font, x, y, "last cmd",
        (state->last_upload && state->last_upload[0]) ? state->last_upload : "-",
        state->last_upload_tone == DglabCmdTone_Error ? kError :
            (state->last_upload_tone == DglabCmdTone_Warn ? kWarn : kText));
    y += LINE_HEIGHT;

    // What to do with it, in one sentence each.
    y += LINE_HEIGHT;
    drawWrapped(canvas, font, x, y, (PANEL_WIDTH - 32) / 16,
        "Move a Joy-Con: the harder it is moved, the stronger and denser its channel becomes. "
        "Both fall back to silence when it is still.",
        kMuted);

    // One block for the safety notes: this mode drives a device that is attached
    // to a body, so they are not footnotes. The sleep warning is part of the same
    // text, otherwise the two would overlap depending on how the wrapping falls.
    snprintf(buffer, sizeof(buffer),
        "This mode sets the waveform only: the volume row above is the channel strength it is "
        "scaled by, and that is set in Socket test. The device really does output current.%s",
        state->server_running
            ? " Do not sleep while the server holds its socket: press Y in Socket test first."
            : "");

    drawWrapped(canvas, font, MARGIN + 16, TITLE_HEIGHT + GAP + PANEL_HEIGHT + 40,
        (SCREEN_WIDTH - 2 * MARGIN - 32) / 16, buffer, kWarn);

    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 56, 1,
        "B clear both channels", kText);
    dglabCanvasText(canvas, font, MARGIN, SCREEN_HEIGHT - 32, 1,
        "+ back to the menu", kText);
}
