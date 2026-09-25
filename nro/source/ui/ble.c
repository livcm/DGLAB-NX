#include <dglab/ui/ble.h>

#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>

// The rows the page is made of: what the session is doing, which device it is
// talking to, and the two numbers this side controls or tracks. The two notes
// under them say the things the rows cannot: that the strength is what we asked
// for, and what starting actually does.
#define BLE_ROW_COUNT 7

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

static int buildRows(const DglabBlePageState* state, DglabRow* rows, char* device,
    size_t device_size, char* limit, size_t limit_size, char* strength, size_t strength_size,
    char* packets, size_t packets_size)
{
    const DglabTheme* theme = dglabThemeGet();
    int count = 0;

    deviceText(&state->status, device, device_size);
    snprintf(limit, limit_size, "%u", (unsigned)state->soft_limit);
    snprintf(strength, strength_size, "A %u / B %u", (unsigned)state->status.strength_a,
        (unsigned)state->status.strength_b);
    snprintf(packets, packets_size, "%u", (unsigned)state->status.packets);

    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleState),
        stateName(state->status.state), NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleDevice), device, NULL,
        theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleSoftLimit), limit, NULL,
        theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BleStrength), strength,
        NULL, theme->text };
    rows[count++] = (DglabRow){ DglabRow_Item, dglabString(DglabString_BlePackets), packets, NULL,
        theme->text };
    rows[count++] = (DglabRow){ DglabRow_Paragraph, NULL, NULL,
        dglabString(DglabString_BleOpenLoop), theme->text };
    rows[count++] = (DglabRow){ DglabRow_Paragraph, NULL, NULL,
        dglabString(DglabString_BleProcedure), theme->text };

    return count;
}

int dglabBleContentHeight(const DglabFontSet* fonts, const DglabBlePageState* state)
{
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    DglabRow rows[BLE_ROW_COUNT];
    DglabRowBox boxes[BLE_ROW_COUNT];
    char device[32];
    char limit[8];
    char strength[32];
    char packets[16];
    int count = buildRows(state, rows, device, sizeof(device), limit, sizeof(limit), strength,
        sizeof(strength), packets, sizeof(packets));

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
    DglabHint hints[2];
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    DglabListPage page;
    char device[32];
    char limit[8];
    char strength[32];
    char packets[16];
    int count = buildRows(state, rows, device, sizeof(device), limit, sizeof(limit), strength,
        sizeof(strength), packets, sizeof(packets));
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height = dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH,
        boxes, BLE_ROW_COUNT);

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

    // Start and stop are the page's actions; B is the back key everywhere, so the
    // bar does not repeat it. Up and down change the soft limit, which the row
    // above shows - a hint would only repeat the row.
    hints[0] = (DglabHint){ DglabButton_A, DglabButton_None,
        dglabString(DglabString_ActionStart), };
    hints[1] = (DglabHint){ DglabButton_X, DglabButton_None,
        dglabString(DglabString_ActionStop), };
    dglabPageHints(canvas, fonts->icon, fonts->value, hints, 2);
}
