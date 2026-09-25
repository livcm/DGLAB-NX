#include <dglab/ui/menu.h>

#include <dglab/ui/list.h>
#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <string.h>

// The mode menu: one row per mode, the description of the selected one under it,
// and the sysmodule's liveness in the header - every mode needs it, and finding
// out only after entering one wastes a trip through the menu. The header's status
// is the same call every other page makes, so the line means one thing.
//
// A row's name is the page's own title, read from the same string, so "the list
// says X and the page says Y" cannot happen.
//
// It is a navigation list, so the selected row gets the console's accent bar as
// well as the focus ring (docs/nro-ui.md).

static const DglabString kItemKeys[DglabMenu_ItemCount] = {
    [DglabMenu_ItemSocket] = DglabString_SocketTitle,
    [DglabMenu_ItemBle] = DglabString_BleTitle,
    [DglabMenu_ItemMotion] = DglabString_MotionTitle,
    [DglabMenu_ItemTouch] = DglabString_TouchTitle,
    [DglabMenu_ItemAdvanced] = DglabString_AdvancedTitle,
    [DglabMenu_ItemAbout] = DglabString_AboutTitle,
    [DglabMenu_ItemBlePoc] = DglabString_ItemBlePoc,
};

static const DglabString kDescKeys[DglabMenu_ItemCount] = {
    [DglabMenu_ItemSocket] = DglabString_DescSocket,
    [DglabMenu_ItemBle] = DglabString_DescBle,
    [DglabMenu_ItemMotion] = DglabString_DescMotion,
    [DglabMenu_ItemTouch] = DglabString_DescTouch,
    [DglabMenu_ItemAdvanced] = DglabString_DescAdvanced,
    [DglabMenu_ItemAbout] = DglabString_DescAbout,
    [DglabMenu_ItemBlePoc] = DglabString_DescBlePoc,
};

const char* dglabMenuItemName(unsigned item)
{
    if (item >= (unsigned)DglabMenu_ItemCount)
        return "?";

    return dglabString(kItemKeys[item]);
}

const char* dglabMenuItemDescription(unsigned item)
{
    if (item >= (unsigned)DglabMenu_ItemCount)
        return "";

    return dglabString(kDescKeys[item]);
}

unsigned dglabMenuMove(unsigned selected, int delta)
{
    int count = (int)DglabMenu_ItemCount;
    int value = (int)selected + delta;

    // Clamped, not wrapped: the list is a list, and stepping off either end used
    // to jump to the other one, which reads as a lost keypress.
    if (value < 0)
        value = 0;

    if (value >= count)
        value = count - 1;

    return (unsigned)value;
}

void dglabMenuDraw(DglabCanvas* canvas, const DglabFontSet* fonts, const DglabMenuState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    // One row per mode, plus the note under the selected one.
    DglabRow rows[DglabMenu_ItemCount + 1];
    DglabRowBox boxes[DglabMenu_ItemCount + 1];
    DglabHint hints[2];
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    DglabListPage page;
    unsigned selected = state ? state->selected : 0;
    bool ok = state ? state->sysmodule_ok : true;
    int count = 0;
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height;

    if (selected >= (unsigned)DglabMenu_ItemCount)
        selected = 0;

    memset(rows, 0, sizeof(rows));

    for (unsigned item = 0; item < (unsigned)DglabMenu_ItemCount; item++) {
        rows[count++] = (DglabRow){
            .kind = DglabRow_Item,
            .label = dglabMenuItemName(item),
            .value_color = theme->accent,
        };

        if (item == selected)
            rows[count++] = (DglabRow){
                .kind = DglabRow_Note,
                .label = dglabMenuItemDescription(item),
            };
    }

    content_height = dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        (int)(sizeof(boxes) / sizeof(boxes[0])));
    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, view_height, content_height, 0);
    // The focus sits on the item row, which is the selected one's own index: the
    // note is only ever added after it.
    page.offset = dglabListScrollFor(0, page.max_offset, view_height, boxes[selected].y,
        boxes[selected].height);

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_MenuTitle));
    dglabPageHeaderStatus(canvas, fonts->value, ok);

    dglabPageClipContent(canvas);

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP - page.offset,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = (int)selected,
        .navigation = true,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, count);

    dglabCanvasClearClip(canvas);

    dglabListPageScrollBar(canvas, &page);

    hints[0] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionExit), };
    hints[1] = (DglabHint){ DglabButton_A, DglabButton_None,
        dglabString(DglabString_ActionEnter), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 2);
}
