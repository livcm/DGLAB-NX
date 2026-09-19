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

// The density axis ends where the two white rules end, so the two numbers have to
// be the page frame's own margin. They live in two layers that cannot include
// each other (the mapping is platform independent), and this is the one place
// both are in scope: change DGLAB_PAGE_MARGIN without the axis and the build
// stops here instead of the picture quietly disagreeing with the output.
_Static_assert(DGLAB_TOUCH_DENSITY_LEFT == DGLAB_PAGE_MARGIN,
    "the density axis must start where the page rules start");
_Static_assert(DGLAB_TOUCH_DENSITY_RIGHT == DGLAB_PAGE_WIDTH - DGLAB_PAGE_MARGIN - 1,
    "the density axis must end where the page rules end");

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

// The grid: the quarter lines of both axes. The ends of the value axis are the
// page's own white rules, and the ends of a half's density axis are the rules'
// ends and the centre line, so only the three lines between them are drawn -
// enough to read a position off the panel by eye without turning the field into
// graph paper. The ticks come from the axis the mapping uses, not from the width
// of the panel: that is what keeps them evenly spaced now that the axis no longer
// starts at the edge of the screen.
static void drawGrid(DglabCanvas* canvas)
{
    const DglabTheme* theme = dglabThemeGet();
    int field_height = TOUCH_FIELD_BOTTOM - TOUCH_FIELD_TOP;
    int value_span = DGLAB_TOUCH_VALUE_BOTTOM - DGLAB_TOUCH_VALUE_TOP;
    // One column of each half, so the axis helpers can answer where that half's
    // density axis begins and ends.
    const uint32_t probes[2] = { 0u, (uint32_t)DGLAB_TOUCH_SPLIT };

    for (int step = 1; step < 4; step++) {
        int y = DGLAB_TOUCH_VALUE_TOP + value_span * step / 4;

        dglabCanvasFill(canvas, 0, y, DGLAB_TOUCH_PANEL_WIDTH, 1, theme->muted);
    }

    for (int half = 0; half < 2; half++) {
        uint32_t start = dglabTouchDensityStart(probes[half]);
        int density_span = (int)(dglabTouchDensityEnd(probes[half]) - start);

        for (int step = 1; step < 4; step++) {
            int x = (int)start + density_span * step / 4;

            dglabCanvasFill(canvas, x, TOUCH_FIELD_TOP, 1, field_height, theme->muted);
        }
    }
}

// The field: the two halves' grid, the centre line they are split by, and one
// marker per finger - plus the two lines that close the density axis by joining
// the ends of the white rules, which is what makes the axis ends visible (they
// are not the edges of the screen: a fingertip cannot reach those, so an axis
// drawn to them never reached the frequency parameters - docs/touch-input.md).
// Clipped to the band, so the page header and the bottom bar keep their own
// background no matter what the panel reports - the full width between the two
// rules is the input (nro/AGENTS.md).
static void drawField(DglabCanvas* canvas, const DglabTouchScreenState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    int field_height = TOUCH_FIELD_BOTTOM - TOUCH_FIELD_TOP;

    dglabCanvasSetClip(canvas, 0, TOUCH_FIELD_TOP, DGLAB_TOUCH_PANEL_WIDTH, field_height);

    drawGrid(canvas);

    dglabCanvasFill(canvas, DGLAB_TOUCH_DENSITY_LEFT, TOUCH_FIELD_TOP, 1, field_height,
        theme->rule);
    dglabCanvasFill(canvas, DGLAB_TOUCH_SPLIT, TOUCH_FIELD_TOP, 1, field_height, theme->rule);
    dglabCanvasFill(canvas, DGLAB_TOUCH_DENSITY_RIGHT, TOUCH_FIELD_TOP, 1, field_height,
        theme->rule);

    if (state->held_a)
        drawMarker(canvas, state->x_a, state->y_a);

    if (state->held_b)
        drawMarker(canvas, state->x_b, state->y_b);

    dglabCanvasClearClip(canvas);
}

// A docked console cannot be touched at all - the panel is inside the dock - and
// there is nothing to point at, so the field is left empty instead of being drawn
// dead: no grid, no lines, no markers. The line that says why used to be a grey
// row under the channels, which is easy to miss on a TV, so it is now a title
// sized line in the space under the rows - where the rows are measured, not
// guessed, so a change to them cannot push the line out of the page. The bottom
// bar already says B leaves (hardware request, 2026-09-19).
static void drawDockedNotice(DglabCanvas* canvas, const DglabFontSet* fonts, int rows_bottom)
{
    const DglabTheme* theme = dglabThemeGet();
    const char* text = dglabString(DglabString_TouchDocked);
    int width = dglabTextWidth(fonts->title, text);
    int top = rows_bottom > TOUCH_FIELD_TOP ? rows_bottom : TOUCH_FIELD_TOP;
    int y = (top + TOUCH_FIELD_BOTTOM) / 2 - fonts->title->cell_height / 2;

    dglabCanvasSetClip(canvas, 0, TOUCH_FIELD_TOP, DGLAB_TOUCH_PANEL_WIDTH,
        TOUCH_FIELD_BOTTOM - TOUCH_FIELD_TOP);

    dglabTextDraw(canvas, fonts->title, (DGLAB_TOUCH_PANEL_WIDTH - width) / 2, y, text,
        theme->text);

    dglabCanvasClearClip(canvas);
}

// One half's live line: where the finger is, in the two numbers the position
// means. Both are the values the slots carry, so the row is the same thing the
// device is being told.
static const char* channelText(bool held, unsigned level, unsigned frequency, char* buffer,
    size_t size)
{
    if (!held)
        snprintf(buffer, size, "%s", dglabString(DglabString_TouchNotTouched));
    else
        snprintf(buffer, size, "%s %u  %s %ums", dglabString(DglabString_MotionLevel), level,
            dglabString(DglabString_TouchDensity), frequency);

    return buffer;
}

void dglabTouchScreenDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabTouchScreenState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[5];
    DglabRowBox boxes[5];
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
    int origin_y = DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + 8;
    int x = DGLAB_PAGE_CONTENT_X;

    snprintf(strength_a, sizeof(strength_a), "%u/100", state->channel_strength_a);
    snprintf(strength_b, sizeof(strength_b), "%u/100", state->channel_strength_b);
    snprintf(label_a, sizeof(label_a), "%s A", dglabString(DglabString_MotionVolume));
    snprintf(label_b, sizeof(label_b), "%s B", dglabString(DglabString_MotionVolume));

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
        .value = channelText(state->held_a, state->level_a, state->frequency_a, left,
            sizeof(left)),
        .value_color = state->held_a ? theme->accent : theme->muted,
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_TouchHalfRight),
        .value = channelText(state->held_b, state->level_b, state->frequency_b, right,
            sizeof(right)),
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

    // Measured before anything is drawn: the field needs the rows' bottom to know
    // where the docked line goes, and the rows themselves are drawn last, over
    // the field.
    dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        (int)(sizeof(boxes) / sizeof(boxes[0])));

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_TouchTitle));
    dglabPageHeaderStatus(canvas, fonts->value, state->sysmodule_ok);

    if (state->docked)
        drawDockedNotice(canvas, fonts,
            origin_y + boxes[count - 1].y + boxes[count - 1].height);
    else
        drawField(canvas, state);

    dglabPageClipContent(canvas);

    for (int i = 0; i < 2; i++) {
        dglabHintDraw(canvas, fonts->icon, fonts->note, &adjust[i], x, DGLAB_PAGE_CONTENT_TOP,
            theme->text);
        x += dglabHintWidth(fonts->note, &adjust[i]) + DGLAB_PAGE_HINT_GAP;
    }

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = origin_y,
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
