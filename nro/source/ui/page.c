#include <dglab/ui/page.h>

#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

void dglabPageBegin(DglabCanvas* canvas)
{
    const DglabTheme* theme = dglabThemeGet();
    int rule_width = DGLAB_PAGE_WIDTH - DGLAB_PAGE_MARGIN * 2;

    dglabCanvasFill(canvas, 0, 0, DGLAB_PAGE_WIDTH, DGLAB_PAGE_HEIGHT, theme->background);
    dglabCanvasHLine(canvas, DGLAB_PAGE_MARGIN, DGLAB_PAGE_RULE_Y, rule_width, theme->rule);
    // The bottom bar's rule is white like the title's; the console keeps the
    // grey rules for the rows inside the page.
    dglabCanvasHLine(canvas, DGLAB_PAGE_MARGIN, DGLAB_PAGE_BAR_Y, rule_width, theme->rule);
}

void dglabPageHeader(DglabCanvas* canvas, const DglabTextStyle* title, const char* title_text)
{
    if (title && title->font && title_text) {
        // The title sits in the middle of the 88px header, measured in the font
        // that draws it: the console centres it, and a fixed y put it low.
        dglabTextDraw(canvas, title->font, DGLAB_PAGE_TITLE_X,
            (DGLAB_PAGE_RULE_Y - title->font->cell_height) / 2, title_text, title->color);
    }
}

void dglabPageHeaderStatus(DglabCanvas* canvas, DglabGlyphSource* font, bool sysmodule_ok)
{
    const DglabTheme* theme = dglabThemeGet();
    const char* text = dglabString(sysmodule_ok ? DglabString_SysmoduleOk
                                                : DglabString_SysmoduleDown);

    if (font == NULL)
        return;

    dglabTextDraw(canvas, font, DGLAB_PAGE_HINTS_RIGHT - dglabTextWidth(font, text),
        (DGLAB_PAGE_RULE_Y - font->cell_height) / 2, text,
        sysmodule_ok ? theme->accent : theme->error);
}

int dglabHintWidth(DglabGlyphSource* font, const DglabHint* hint)
{
    int width = dglabButtonIconWidth(hint->button);

    if (hint->button2 != DglabButton_None)
        width += DGLAB_BUTTON_ICON_GAP + dglabButtonIconWidth(hint->button2);

    return width + DGLAB_PAGE_HINT_TEXT_GAP + dglabTextWidth(font, hint->action);
}

void dglabHintDraw(DglabCanvas* canvas, DglabGlyphSource* icon_font, DglabGlyphSource* font,
    const DglabHint* hint, int x, int y, uint32_t color)
{
    int text_y = y + (DGLAB_BUTTON_ICON_HEIGHT - font->cell_height) / 2;
    int width = dglabButtonIconWidth(hint->button);

    dglabButtonIcon(canvas, icon_font, hint->button, x, y, color);

    if (hint->button2 != DglabButton_None) {
        x += width + DGLAB_BUTTON_ICON_GAP;
        width = dglabButtonIconWidth(hint->button2);
        dglabButtonIcon(canvas, icon_font, hint->button2, x, y, color);
    }

    dglabTextDraw(canvas, font, x + width + DGLAB_PAGE_HINT_TEXT_GAP, text_y, hint->action,
        color);
}

void dglabPageHints(DglabCanvas* canvas, DglabGlyphSource* icon_font, DglabGlyphSource* font,
    const DglabHint* hints, int count)
{
    int x = DGLAB_PAGE_HINTS_RIGHT;

    if (font == NULL || hints == NULL || count <= 0)
        return;

    // From the right inwards, so the last hint in reading order ends at the
    // margin and the group grows to the left.
    for (int i = count - 1; i >= 0; i--) {
        int hint_width = dglabHintWidth(font, &hints[i]);

        x -= hint_width;

        dglabHintDraw(canvas, icon_font, font, &hints[i], x, DGLAB_PAGE_HINT_ICON_Y,
            dglabThemeGet()->text);

        x -= DGLAB_PAGE_HINT_GAP;
    }
}

void dglabPageClipContent(DglabCanvas* canvas)
{
    dglabCanvasSetClip(canvas, DGLAB_PAGE_CONTENT_X, DGLAB_PAGE_CLIP_TOP,
        DGLAB_PAGE_CONTENT_WIDTH, DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CLIP_TOP);
}

void dglabPageClipWide(DglabCanvas* canvas)
{
    dglabCanvasSetClip(canvas, DGLAB_PAGE_WIDE_X, DGLAB_PAGE_CLIP_TOP,
        DGLAB_PAGE_WIDE_WIDTH, DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CLIP_TOP);
}
