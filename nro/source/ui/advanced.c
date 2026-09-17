#include <dglab/ui/advanced.h>

#include <dglab/ui/list.h>
#include <dglab/ui/page.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <string.h>

// Every motion parameter on one page: one row per setting (name, value) with the
// explanation of the selected one under it. The cursor moves with up and down
// and the value changes with left and right, which is the one hint the bottom
// bar carries; the page scrolls with the cursor, so the selected row and its
// explanation always stay in sight.

static const DglabString kNameKeys[DglabMotionSetting_Count] = {
    [DglabMotionSetting_DeadzoneEnter] = DglabString_SetDeadzoneEnter,
    [DglabMotionSetting_DeadzoneExit] = DglabString_SetDeadzoneExit,
    [DglabMotionSetting_GyroRange] = DglabString_SetGyroRange,
    [DglabMotionSetting_AccelRange] = DglabString_SetAccelRange,
    [DglabMotionSetting_GyroWeight] = DglabString_SetGyroWeight,
    [DglabMotionSetting_AccelWeight] = DglabString_SetAccelWeight,
    [DglabMotionSetting_Attack] = DglabString_SetAttack,
    [DglabMotionSetting_Release] = DglabString_SetRelease,
    [DglabMotionSetting_IdleStop] = DglabString_SetIdleStop,
    [DglabMotionSetting_FrequencyFast] = DglabString_SetFrequencyFast,
    [DglabMotionSetting_FrequencyStill] = DglabString_SetFrequencyStill,
    [DglabMotionSetting_StrengthMax] = DglabString_SetStrengthMax,
};

static const DglabString kDescKeys[DglabMotionSetting_Count] = {
    [DglabMotionSetting_DeadzoneEnter] = DglabString_DescDeadzoneEnter,
    [DglabMotionSetting_DeadzoneExit] = DglabString_DescDeadzoneExit,
    [DglabMotionSetting_GyroRange] = DglabString_DescGyroRange,
    [DglabMotionSetting_AccelRange] = DglabString_DescAccelRange,
    [DglabMotionSetting_GyroWeight] = DglabString_DescWeight,
    [DglabMotionSetting_AccelWeight] = DglabString_DescWeight,
    [DglabMotionSetting_Attack] = DglabString_DescAttack,
    [DglabMotionSetting_Release] = DglabString_DescRelease,
    [DglabMotionSetting_IdleStop] = DglabString_DescIdleStop,
    [DglabMotionSetting_FrequencyFast] = DglabString_DescFrequencyFast,
    [DglabMotionSetting_FrequencyStill] = DglabString_DescFrequencyStill,
    [DglabMotionSetting_StrengthMax] = DglabString_DescStrengthMax,
};

void dglabAdvancedDraw(DglabCanvas* canvas, const DglabFontSet* fonts,
    const DglabAdvancedState* state)
{
    const DglabTheme* theme = dglabThemeGet();
    DglabListFonts list_fonts = { fonts->body, fonts->value, fonts->note };
    // One row per setting, plus the explanation of the selected one and the
    // line that says whether the change reached the config file.
    DglabRow rows[DglabMotionSetting_Count + 2];
    DglabRowBox boxes[DglabMotionSetting_Count + 2];
    DglabHint hints[3];
    DglabTextStyle title = { fonts->title, theme->text };
    DglabListStyle style;
    unsigned selected = state->selected;
    // One buffer per setting: the rows are drawn after the whole page is built,
    // so a single shared buffer would leave every row showing the last value.
    char values[DglabMotionSetting_Count][32];
    int focus = 0;
    int count = 0;
    int view_height = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content_height;
    int max_offset;
    int offset;

    if (selected >= (unsigned)DglabMotionSetting_Count)
        selected = 0;

    memset(rows, 0, sizeof(rows));

    for (unsigned setting = 0; setting < (unsigned)DglabMotionSetting_Count; setting++) {
        dglabMotionSettingsFormat(state->config, setting, values[setting],
            sizeof(values[setting]));

        if (setting == selected)
            focus = count;

        rows[count++] = (DglabRow){
            .kind = DglabRow_Item,
            .label = dglabString(kNameKeys[setting]),
            .value = values[setting],
            .value_color = setting == selected ? theme->accent : theme->text,
            .note = setting == selected ? dglabString(kDescKeys[setting]) : NULL,
        };
    }

    rows[count++] = (DglabRow){
        .kind = DglabRow_Note,
        .label = state->saved ? dglabString(DglabString_AdvancedSaved)
                              : dglabString(DglabString_AdvancedSaveFailed),
    };

    content_height = dglabListMeasure(&list_fonts, rows, count, DGLAB_PAGE_CONTENT_WIDTH, boxes,
        (int)(sizeof(boxes) / sizeof(boxes[0])));
    max_offset = content_height > view_height ? content_height - view_height : 0;
    // The explanation belongs to the selected row, so the page scrolls to the
    // pair rather than to the row alone.
    offset = dglabListScrollFor(0, max_offset, view_height, boxes[focus].y,
        boxes[focus].height);

    dglabPageBegin(canvas);
    dglabPageHeader(canvas, &title, dglabString(DglabString_AdvancedTitle), NULL, NULL);
    dglabPageClipContent(canvas);

    style = (DglabListStyle){
        .x = DGLAB_PAGE_CONTENT_X,
        .origin_y = DGLAB_PAGE_CONTENT_TOP - offset,
        .width = DGLAB_PAGE_CONTENT_WIDTH,
        .focus = focus,
        .navigation = false,
    };
    dglabListDraw(canvas, &list_fonts, &style, rows, boxes, count);

    dglabCanvasClearClip(canvas);

    dglabListScrollBar(canvas, DGLAB_PAGE_CONTENT_TOP, view_height, content_height, offset);

    hints[0] = (DglabHint){ DglabButton_Left, DglabButton_Right,
        dglabString(DglabString_ActionAdjust), };
    hints[1] = (DglabHint){ DglabButton_B, DglabButton_None,
        dglabString(DglabString_ActionBack), };
    hints[2] = (DglabHint){ DglabButton_Y, DglabButton_None,
        dglabString(DglabString_ActionReset), };
    dglabPageHints(canvas, fonts->icon, fonts->body, hints, 3);
}
