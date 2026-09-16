#include <dglab/ui/motion.h>

#include <dglab/ui/list.h>
#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

// The motion mode is a read-only page with the same shortcuts as the socket
// page: the D-pad dials the two channel strengths, ZL and ZR fire a test, X
// clears what a test left playing. Nothing is focused and nothing scrolls, so
// the page has to fit the content column as it is.

static uint32_t toneColor(u32 tone)
{
    const DglabTheme* theme = dglabThemeGet();

    switch (tone) {
        case DglabCmdTone_Warn: return theme->warn;
        case DglabCmdTone_Error: return theme->error;
        default: return theme->text;
    }
}

// One channel's live line: whether that side is connected, how hard it is being
// moved, and what the mapping is sending because of it.
static const char* channelText(const DglabMotionScreenState* state, bool left, char* buffer,
    size_t size, bool* connected)
{
    bool is_connected = left ? state->left_connected : state->right_connected;
    bool moving = left ? state->moving_a : state->moving_b;
    unsigned level = left ? state->level_a : state->level_b;
    unsigned frequency = left ? state->frequency_a : state->frequency_b;

    *connected = is_connected;

    if (!is_connected) {
        snprintf(buffer, size, "%s", dglabString(DglabString_MotionNotConnected));
    } else if (!moving) {
        snprintf(buffer, size, "%s   %s 0   %ums", dglabString(DglabString_MotionStill),
            dglabString(DglabString_MotionLevel), frequency);
    } else {
        snprintf(buffer, size, "%s   %s %u   %ums", dglabString(DglabString_MotionMoving),
            dglabString(DglabString_MotionLevel), level, frequency);
    }

    return buffer;
}

void dglabMotionScreenDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabMotionScreenState* state)
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
    char channel_a[96];
    char channel_b[96];
    char strength_a[32];
    char strength_b[32];
    char label_a[64];
    char label_b[64];
    bool connected_a;
    bool connected_b;
    int x = DGLAB_PAGE_CONTENT_X;

    snprintf(strength_a, sizeof(strength_a), "%u/100", state->channel_strength_a);
    snprintf(strength_b, sizeof(strength_b), "%u/100", state->channel_strength_b);
    // The two strengths this waveform is scaled by, one row each.
    snprintf(label_a, sizeof(label_a), "%s A", dglabString(DglabString_MotionVolume));
    snprintf(label_b, sizeof(label_b), "%s B", dglabString(DglabString_MotionVolume));

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_MotionTitle), NULL, NULL);
    dglabPageClipContent(canvas);

    for (int i = 0; i < 2; i++) {
        dglabHintDraw(canvas, fonts->icon, fonts->note, &adjust[i], x, DGLAB_PAGE_CONTENT_TOP,
            theme->text);
        x += dglabHintWidth(fonts->note, &adjust[i]) + DGLAB_PAGE_HINT_GAP;
    }

    memset(rows, 0, sizeof(rows));

    rows[0] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_MotionLink),
        .value = state->link ? state->link : "-",
        .value_color = toneColor(state->link_tone),
    };
    rows[1] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_MotionJoyConLeft),
        .value = channelText(state, true, channel_a, sizeof(channel_a), &connected_a),
        .value_color = connected_a ? theme->accent : theme->muted,
    };
    rows[2] = (DglabRow){
        .kind = DglabRow_Item,
        .label = dglabString(DglabString_MotionJoyConRight),
        .value = channelText(state, false, channel_b, sizeof(channel_b), &connected_b),
        .value_color = connected_b ? theme->accent : theme->muted,
    };
    rows[3] = (DglabRow){
        .kind = DglabRow_Item,
        .label = label_a,
        .value = strength_a,
        .value_color = theme->text,
    };
    rows[4] = (DglabRow){
        .kind = DglabRow_Item,
        .label = label_b,
        .value = strength_b,
        .value_color = theme->text,
    };

    dglabListMeasure(&list_fonts, rows, 5, DGLAB_PAGE_CONTENT_WIDTH, boxes, 5);

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + 8,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = -1,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, 5);

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
