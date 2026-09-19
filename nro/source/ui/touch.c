#include <dglab/ui/touch.h>

#include <dglab/ui/list.h>
#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

// The touch mode: the whole band between the two white rules is the input, so
// the page draws its two halves there - the split down the middle and where each
// finger is - and keeps its rows and hints where every other page keeps them.
//
// Nothing is focused and nothing scrolls: the rows are a handful of fixed lines,
// which is why the mode can spend the rest of the band on the field.

// The finger marker: a filled disc with a ring around it, so it stays visible
// over the centre line and the grid.
#define TOUCH_MARKER_RADIUS 12
#define TOUCH_MARKER_RING 22
#define TOUCH_MARKER_RING_WIDTH 2
// The band the field occupies: one pixel under the title rule through the last
// row above the bottom bar's rule. A marker is clamped inside it, so a finger on
// the header still shows up instead of being clipped away.
#define TOUCH_FIELD_TOP DGLAB_PAGE_CLIP_TOP
#define TOUCH_FIELD_BOTTOM (DGLAB_PAGE_BAR_Y - 1)

static uint32_t toneColor(u32 tone)
{
    const DglabTheme* theme = dglabThemeGet();

    switch (tone) {
        case DglabCmdTone_Warn: return theme->warn;
        case DglabCmdTone_Error: return theme->error;
        default: return theme->text;
    }
}

static int clampInt(int value, int min, int max)
{
    if (value < min)
        return min;

    if (value > max)
        return max;

    return value;
}

static void drawMarker(DglabCanvas* canvas, unsigned x, unsigned y)
{
    const DglabTheme* theme = dglabThemeGet();
    int cx = clampInt((int)x, TOUCH_MARKER_RING, DGLAB_TOUCH_PANEL_WIDTH - 1 - TOUCH_MARKER_RING);
    int cy = clampInt((int)y, TOUCH_FIELD_TOP + TOUCH_MARKER_RING,
        TOUCH_FIELD_BOTTOM - TOUCH_MARKER_RING);

    dglabCanvasDisc(canvas, cx, cy, TOUCH_MARKER_RADIUS, theme->accent);
    dglabCanvasRing(canvas, cx, cy, TOUCH_MARKER_RING, TOUCH_MARKER_RING_WIDTH, theme->accent);
}

// The field: the centre line the halves are split by, and one marker per finger.
// Clipped to the band, so the page header and the bottom bar keep their own
// background no matter what the panel reports.
static void drawField(DglabCanvas* canvas, const DglabTouchScreenState* state)
{
    const DglabTheme* theme = dglabThemeGet();

    dglabCanvasSetClip(canvas, 0, TOUCH_FIELD_TOP, DGLAB_TOUCH_PANEL_WIDTH,
        TOUCH_FIELD_BOTTOM - TOUCH_FIELD_TOP);

    dglabCanvasFill(canvas, DGLAB_TOUCH_SPLIT, TOUCH_FIELD_TOP, 1,
        TOUCH_FIELD_BOTTOM - TOUCH_FIELD_TOP, theme->rule);

    if (state->held_a)
        drawMarker(canvas, state->x_a, state->y_a);

    if (state->held_b)
        drawMarker(canvas, state->x_b, state->y_b);

    dglabCanvasClearClip(canvas);
}

// One half's live line. The probe build shows where the panel says the finger
// is; what the mode makes of that position is the next step (docs/touch-input.md).
static const char* channelText(bool held, unsigned x, unsigned y, char* buffer, size_t size)
{
    if (!held)
        snprintf(buffer, size, "%s", dglabString(DglabString_TouchNotTouched));
    else
        snprintf(buffer, size, "x %u  y %u", x, y);

    return buffer;
}

void dglabTouchScreenDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabTouchScreenState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[6];
    DglabRowBox boxes[6];
    DglabHint hints[4];
    DglabHint adjust[2] = {
        { DglabButton_Up, DglabButton_Down, dglabString(DglabString_HintAdjustA) },
        { DglabButton_Left, DglabButton_Right, dglabString(DglabString_HintAdjustB) },
    };
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    char left[64];
    char right[64];
    char strength_a[32];
    char strength_b[32];
    char label_a[64];
    char label_b[64];
    int count = 0;
    int x = DGLAB_PAGE_CONTENT_X;

    snprintf(strength_a, sizeof(strength_a), "%u/100", state->channel_strength_a);
    snprintf(strength_b, sizeof(strength_b), "%u/100", state->channel_strength_b);
    snprintf(label_a, sizeof(label_a), "%s A", dglabString(DglabString_MotionVolume));
    snprintf(label_b, sizeof(label_b), "%s B", dglabString(DglabString_MotionVolume));

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_TouchTitle));
    dglabPageHeaderStatus(canvas, fonts->value, state->sysmodule_ok);

    drawField(canvas, state);

    dglabPageClipContent(canvas);

    for (int i = 0; i < 2; i++) {
        dglabHintDraw(canvas, fonts->icon, fonts->note, &adjust[i], x, DGLAB_PAGE_CONTENT_TOP,
            theme->text);
        x += dglabHintWidth(fonts->note, &adjust[i]) + DGLAB_PAGE_HINT_GAP;
    }

    memset(rows, 0, sizeof(rows));

    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_MotionLink),
        .value = state->link ? state->link : "-",
        .value_color = toneColor(state->link_tone),
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_TouchHalfLeft),
        .value = channelText(state->held_a, state->x_a, state->y_a, left, sizeof(left)),
        .value_color = state->held_a ? theme->accent : theme->muted,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_TouchHalfRight),
        .value = channelText(state->held_b, state->x_b, state->y_b, right, sizeof(right)),
        .value_color = state->held_b ? theme->accent : theme->muted,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = label_a,
        .value = strength_a,
        .value_color = theme->text,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = label_b,
        .value = strength_b,
        .value_color = theme->text,
    };

    // A docked console cannot be touched at all, which is worth a line of its
    // own: without it the mode looks like it stopped working.
    if (state->docked)
        rows[count++] = (DglabRow){
            .kind = DglabRow_Note,
            .label = dglabString(DglabString_TouchDocked),
        };

    dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        (int)(sizeof(boxes) / sizeof(boxes[0])));

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + 8,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = -1,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, count);

    dglabCanvasClearClip(canvas);

    hints[0] = (DglabHint){ DglabButton_ZL, DglabButton_ZR,
        dglabString(DglabString_ActionTestChannels), };
    hints[1] = (DglabHint){ DglabButton_X, DglabButton_None,
        dglabString(DglabString_ActionClear), };
    hints[2] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionBack), };
    hints[3] = (DglabHint){ DglabButton_A, DglabButton_None,
        state->server_running ? dglabString(DglabString_ActionStop)
                              : dglabString(DglabString_ActionStart), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 4);
}
