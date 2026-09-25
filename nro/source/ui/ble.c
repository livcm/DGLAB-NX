#include <dglab/ui/ble.h>

#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

// The rows the page is made of: what the session is doing, which device it is
// talking to, the two strengths the D-pad dials (each one showing the ceiling it
// stops at after the value), and how many packets went out. The two channel
// ceilings are not rows of their own: they are settings the parameters page
// owns, and they are already on screen as the second half of those two values.
//
// The page fits one screen on purpose. Up and down are channel A's keys here, so
// there is no key left to scroll with, and the project's rule is that a page
// without a scroll key carries no more than a screen.
#define BLE_ROW_COUNT 5

static const char* stateName(u32 state)
{
    switch (state) {
        case DglabBleState_Connecting: return dglabString(DglabString_BleStateConnecting);
        case DglabBleState_Connected: return dglabString(DglabString_BleStateConnected);
        case DglabBleState_Failed: return dglabString(DglabString_BleStateFailed);
        default: return dglabString(DglabString_BleStateIdle);
    }
}

static void deviceText(const DglabBleStatus* status, char* out, size_t size)
{
    bool any = false;

    for (int i = 0; i < 6; i++) {
        if (status->address[i] != 0)
            any = true;
    }

    if (!any) {
        snprintf(out, size, "%s", dglabString(DglabString_BleDeviceNone));
        return;
    }

    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", status->address[0], status->address[1],
        status->address[2], status->address[3], status->address[4], status->address[5]);
}

// One buffer per row: the rows are drawn after the whole list is built, so a
// single shared buffer would leave every row showing the last value.
typedef struct {
    char device[32];
    char strength_a[16];
    char strength_b[16];
    char packets[16];
    char label_a[64];
    char label_b[64];
} BleRowText;

static int buildRows(const DglabBlePageState* state, DglabRow* rows, BleRowText* text)
{
    const DglabTheme* theme = dglabThemeGet();
    int count = 0;

    deviceText(&state->status, text->device, sizeof(text->device));
    // value/ceiling, like the socket and gameplay pages: the strength the D-pad
    // dials, over the channel ceiling it stops at.
    snprintf(text->strength_a, sizeof(text->strength_a), "%u/%u", (unsigned)state->strength_a,
        (unsigned)state->limit_a);
    snprintf(text->strength_b, sizeof(text->strength_b), "%u/%u", (unsigned)state->strength_b,
        (unsigned)state->limit_b);
    snprintf(text->packets, sizeof(text->packets), "%u", (unsigned)state->status.packets);
    // The two channels are named the way the motion page names them, so the
    // strength that is dialled reads the same on every page that dials it.
    snprintf(text->label_a, sizeof(text->label_a), "%s A", dglabString(DglabString_MotionVolume));
    snprintf(text->label_b, sizeof(text->label_b), "%s B", dglabString(DglabString_MotionVolume));

    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleState),
        stateName(state->status.state), NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleDevice), text->device,
        NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, text->label_a, text->strength_a, NULL,
        theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, text->label_b, text->strength_b, NULL,
        theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BlePackets), text->packets,
        NULL, theme->text };
    return count;
}

void dglabBleDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabBlePageState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[BLE_ROW_COUNT];
    DglabRowBox boxes[BLE_ROW_COUNT];
    DglabHint hints[4];
    DglabHint adjust[2] = {
        { DglabButton_Up, DglabButton_Down, dglabString(DglabString_HintAdjustA) },
        { DglabButton_Left, DglabButton_Right, dglabString(DglabString_HintAdjustB) },
    };
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    BleRowText text;
    int count;
    int x = DGLAB_PAGE_CONTENT_X;

    memset(&text, 0, sizeof(text));
    count = buildRows(state, rows, &text);
    dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes, BLE_ROW_COUNT);

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_BleTitle));
    dglabPageHeaderStatus(canvas, fonts->value, state->sysmodule_ok);

    dglabPageClipContent(canvas);

    // The two adjustment hints are the line above the rows, exactly where the
    // socket, motion and touch pages put theirs: the bottom bar is for the
    // actions a page has of its own.
    for (int i = 0; i < 2; i++) {
        dglabHintDraw(canvas, fonts->icon, fonts->note, &adjust[i], x, DGLAB_PAGE_CONTENT_TOP,
            theme->text);
        x += dglabHintWidth(fonts->note, &adjust[i]) + DGLAB_PAGE_HINT_GAP;
    }

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + 8,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = -1,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, count);

    dglabCanvasClearClip(canvas);

    hints[0] = (DglabHint){ DglabButton_Y, DglabButton_None,
        dglabString(DglabString_ActionLog), };
    hints[1] = (DglabHint){ DglabButton_A, DglabButton_None,
        dglabString(DglabString_ActionStart), };
    hints[2] = (DglabHint){ DglabButton_X, DglabButton_None,
        dglabString(DglabString_ActionStop), };
    hints[3] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionBack), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 4);
}
