#include <dglab/ui/ble.h>

#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

// The rows the page is made of: what the session is doing, which device it is
// talking to, the two channel strength ceilings the session was started with,
// the two strengths the D-pad dials, and how many packets went out. The two
// notes under them say the things the rows cannot: that the strengths are what
// we asked for, and what starting actually does.
//
// The ceilings are read only here: they are settings, and the advanced
// parameters page is where they are changed (motion_settings.c). Keeping one
// page able to change them would mean two places to look for "why is nothing
// coming out", which is the state the 2026-09-26 run ended in.
#define BLE_ROW_COUNT 9

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
    char limit_a[8];
    char limit_b[8];
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
    snprintf(text->limit_a, sizeof(text->limit_a), "%u", (unsigned)state->limit_a);
    snprintf(text->limit_b, sizeof(text->limit_b), "%u", (unsigned)state->limit_b);
    snprintf(text->strength_a, sizeof(text->strength_a), "%u/100", (unsigned)state->strength_a);
    snprintf(text->strength_b, sizeof(text->strength_b), "%u/100", (unsigned)state->strength_b);
    snprintf(text->packets, sizeof(text->packets), "%u", (unsigned)state->status.packets);
    // The two channels are named the way the motion page names them, so the
    // strength that is dialled reads the same on every page that dials it.
    snprintf(text->label_a, sizeof(text->label_a), "%s A", dglabString(DglabString_MotionVolume));
    snprintf(text->label_b, sizeof(text->label_b), "%s B", dglabString(DglabString_MotionVolume));

    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleState),
        stateName(state->status.state), NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleDevice), text->device,
        NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_SetChannelLimitA),
        text->limit_a, NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_SetChannelLimitB),
        text->limit_b, NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, text->label_a, text->strength_a, NULL,
        theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, text->label_b, text->strength_b, NULL,
        theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BlePackets), text->packets,
        NULL, theme->text };
    // A paragraph row paints its own text out of `label` (nro/include/dglab/ui/list.h).
    // These two used to hand it over in `note`, which the row kind never reads -
    // so the page drew an empty band where its two explanations belong, and
    // nothing said so: the layout checks only look at where ink lands, and "no
    // ink at all" is inside every region (found by rendering the page, see
    // tests/canvas). Spell the field out so the next edit cannot repeat it.
    rows[count++] = (DglabRow){
        .kind = DglabRow_Paragraph,
        .label = dglabString(DglabString_BleOpenLoop),
    };
    rows[count++] = (DglabRow){
        .kind = DglabRow_Paragraph,
        .label = dglabString(DglabString_BleProcedure),
    };

    return count;
}

int dglabBleContentHeight(const DglabFontSet* fonts, const DglabBlePageState* state)
{
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[BLE_ROW_COUNT];
    DglabRowBox boxes[BLE_ROW_COUNT];
    BleRowText text;
    int count;

    memset(&text, 0, sizeof(text));
    count = buildRows(state, rows, &text);

    return dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        BLE_ROW_COUNT);
}

void dglabBleDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabBlePageState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[BLE_ROW_COUNT];
    DglabRowBox boxes[BLE_ROW_COUNT];
    DglabHint hints[5];
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    DglabListPage page;
    BleRowText text;
    int count;
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height;

    memset(&text, 0, sizeof(text));
    count = buildRows(state, rows, &text);
    content_height = dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        BLE_ROW_COUNT);

    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, view_height, content_height, state->offset);

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_BleTitle));
    dglabPageHeaderStatus(canvas, fonts->value, state->sysmodule_ok);

    dglabPageClipContent(canvas);

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP - page.offset,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = -1,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, count);

    dglabCanvasClearClip(canvas);

    dglabListPageScrollBar(canvas, &page);

    // The D-pad is the mixer here too, exactly as on the socket and motion
    // pages, so the bar has to say what it does. B is the back key everywhere,
    // so the bar does not repeat it.
    hints[0] = (DglabHint){ DglabButton_Up, DglabButton_Down,
        dglabString(DglabString_HintAdjustA), };
    hints[1] = (DglabHint){ DglabButton_Left, DglabButton_Right,
        dglabString(DglabString_HintAdjustB), };
    hints[2] = (DglabHint){ DglabButton_Y, DglabButton_None,
        dglabString(DglabString_ActionLog), };
    hints[3] = (DglabHint){ DglabButton_A, DglabButton_None,
        dglabString(DglabString_ActionStart), };
    hints[4] = (DglabHint){ DglabButton_X, DglabButton_None,
        dglabString(DglabString_ActionStop), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 5);
}
