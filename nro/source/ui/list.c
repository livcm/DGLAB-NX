#include <dglab/ui/list.h>

#include <dglab/ui/page.h>
#include <dglab/ui/theme.h>

#include <string.h>

// The bullet the console puts in front of a note. It is one glyph from the
// system font, so it needs no drawing code of its own.
#define NOTE_BULLET "\xE2\x97\x86 " // U+25C6
// Space the bullet and its gap take from a note's line.
#define NOTE_INDENT 26

static int noteLines(DglabGlyphSource* note, const char* text, int width)
{
    if (note == NULL || text == NULL || text[0] == '\0')
        return 0;

    if (width < NOTE_INDENT * 2)
        width = NOTE_INDENT * 2;

    return dglabTextCountLines(note, text, width - NOTE_INDENT);
}

static bool rowFocusable(const DglabRow* row)
{
    return row->kind != DglabRow_Note;
}

int dglabListMeasure(const DglabListFonts* fonts, const DglabRow* rows, int count, int width,
    DglabRowBox* boxes, int capacity)
{
    int y = 0;

    if (count > capacity)
        return 0;

    for (int i = 0; i < count; i++) {
        int height;
        int lines = noteLines(fonts->note, rows[i].note, width);

        switch (rows[i].kind) {
            case DglabRow_Note:
                height = DGLAB_NOTE_GAP * 2 +
                    dglabTextCountLines(fonts->note, rows[i].label, width - NOTE_INDENT) *
                        DGLAB_NOTE_LINE;
                break;
            case DglabRow_Paragraph:
                height = DGLAB_PARAGRAPH_GAP * 2 +
                    dglabTextCountLines(fonts->body, rows[i].label, width) * DGLAB_PARAGRAPH_LINE;
                break;
            default:
                height = DGLAB_ROW_HEIGHT;
                break;
        }

        if (lines > 0)
            height += lines * DGLAB_NOTE_LINE + DGLAB_NOTE_GAP;

        boxes[i].y = y;
        boxes[i].height = height;
        y += height;
    }

    return y;
}

// One line of a note's text, indented past the bullet.
static void drawNoteBlock(DglabCanvas* canvas, const DglabListFonts* fonts, int x, int y,
    int width, const char* text)
{
    char line[192];
    int first = 1;

    if (text == NULL)
        return;

    while (*text) {
        size_t taken = dglabTextWrapLine(fonts->note, text, width - NOTE_INDENT, line,
            sizeof(line));

        if (taken == 0)
            break;

        if (first)
            dglabTextDraw(canvas, fonts->note, x, y, NOTE_BULLET, dglabThemeGet()->muted);

        dglabTextDraw(canvas, fonts->note, x + NOTE_INDENT, y, line, dglabThemeGet()->muted);

        y += DGLAB_NOTE_LINE;
        text += taken;
        first = 0;

        while (*text == ' ')
            text++;
    }
}

// A page's own paragraph: the body font, no bullet, wrapped to the column.
static void drawParagraph(DglabCanvas* canvas, const DglabListFonts* fonts, int x, int y,
    int width, const char* text)
{
    char line[192];

    if (text == NULL)
        return;

    while (*text) {
        size_t taken = dglabTextWrapLine(fonts->body, text, width, line, sizeof(line));

        if (taken == 0)
            break;

        dglabTextDraw(canvas, fonts->body, x, y, line, dglabThemeGet()->text);

        y += DGLAB_PARAGRAPH_LINE;
        text += taken;

        while (*text == ' ')
            text++;
    }
}

static void drawItem(DglabCanvas* canvas, const DglabListFonts* fonts, const DglabRow* row,
    int x, int y, int width, bool focused, bool navigation)
{
    const DglabTheme* theme = dglabThemeGet();
    int label_y = y + (DGLAB_ROW_HEIGHT - fonts->body->cell_height) / 2;
    // The console keeps the label white either way and spends the accent on the
    // value; a navigation list is the one place the selected label turns cyan.
    uint32_t label_color = (navigation && focused) ? theme->accent : theme->text;
    // The console insets a row's text from the rule it sits on: the rule runs
    // the full width, the label starts 16px in, and the value keeps the same
    // distance from the other end so nothing touches the focus ring.
    int label_x = x + DGLAB_ROW_PAD + (navigation ? 20 : 0);
    int value_right = x + width - DGLAB_ROW_PAD;

    if (focused) {
        dglabCanvasRoundFill(canvas, x, y - (DGLAB_ROW_FOCUS_HEIGHT - DGLAB_ROW_HEIGHT) / 2,
            width, DGLAB_ROW_FOCUS_HEIGHT, DGLAB_ROW_FOCUS_RADIUS, theme->focus_fill);
        dglabCanvasRoundFrame(canvas, x, y - (DGLAB_ROW_FOCUS_HEIGHT - DGLAB_ROW_HEIGHT) / 2,
            width, DGLAB_ROW_FOCUS_HEIGHT, DGLAB_ROW_FOCUS_RADIUS, DGLAB_ROW_FOCUS_RING,
            theme->focus_ring);

        if (navigation) {
            // The bar the console puts on the selected entry of a navigation
            // list, the one thing that separates "where I am" from "what I am
            // on" in a two column layout.
            dglabCanvasFill(canvas, x + 12, y + (DGLAB_ROW_HEIGHT - fonts->body->cell_height) / 2,
                4, fonts->body->cell_height, theme->accent);
        }
    }

    if (row->label)
        dglabTextDraw(canvas, fonts->body, label_x, label_y, row->label, label_color);

    if (row->value) {
        dglabTextDraw(canvas, fonts->value,
            value_right - dglabTextWidth(fonts->value, row->value),
            y + (DGLAB_ROW_HEIGHT - fonts->value->cell_height) / 2, row->value,
            row->value_color ? row->value_color : theme->accent);
    }
}

void dglabListDraw(DglabCanvas* canvas, const DglabListFonts* fonts, const DglabListStyle* style,
    const DglabRow* rows, const DglabRowBox* boxes, int count)
{
    const DglabTheme* theme = dglabThemeGet();
    int x = style->x;

    for (int i = 0; i < count; i++) {
        int y = style->origin_y + boxes[i].y;
        bool focused = (i == style->focus);

        if (rows[i].kind == DglabRow_Note) {
            drawNoteBlock(canvas, fonts, x, y + DGLAB_NOTE_GAP, style->width, rows[i].label);
            continue;
        }

        if (rows[i].kind == DglabRow_Paragraph) {
            // The console's own words: white body text, no bullet, wrapping to
            // the column. The grey bulleted note is for text attached to a row.
            drawParagraph(canvas, fonts, x, y + DGLAB_PARAGRAPH_GAP, style->width,
                rows[i].label);
            continue;
        }

        // Every row opens with its own rule, which is what makes the last one
        // of the list end on a line as well.
        dglabCanvasHLine(canvas, x, y, style->width, theme->separator);

        drawItem(canvas, fonts, &rows[i], x, y, style->width, focused, style->navigation);

        if (rows[i].note) {
            int note_y = y + DGLAB_ROW_HEIGHT + DGLAB_NOTE_GAP;

            drawNoteBlock(canvas, fonts, x, note_y, style->width, rows[i].note);
        }
    }

    if (count > 0) {
        const DglabRowBox* last = &boxes[count - 1];

        if (rows[count - 1].kind != DglabRow_Note)
            dglabCanvasHLine(canvas, x, style->origin_y + last->y + last->height, style->width,
                theme->separator);
    }
}

int dglabListFocusMove(const DglabRow* rows, int count, int focus, int delta)
{
    int index = focus;

    if (count <= 0)
        return -1;

    if (index < 0)
        index = delta > 0 ? -1 : 0;

    for (int step = 0; step < count; step++) {
        index += delta;

        if (index < 0)
            index = count - 1;

        if (index >= count)
            index = 0;

        if (rowFocusable(&rows[index]))
            return index;
    }

    return focus;
}

int dglabListScrollFor(int offset, int max_offset, int view_height, int row_y, int row_height)
{
    // Room the focus ring needs around the row it is drawn on.
    int overhang = (DGLAB_ROW_FOCUS_HEIGHT - DGLAB_ROW_HEIGHT) / 2 + 2;

    if (row_y - overhang < offset)
        offset = row_y - overhang;

    if (row_y + row_height + overhang > offset + view_height)
        offset = row_y + row_height + overhang - view_height;

    return dglabListScrollClamp(offset, max_offset);
}

int dglabListScrollClamp(int offset, int max_offset)
{
    if (max_offset < 0)
        max_offset = 0;

    if (offset < 0)
        offset = 0;

    if (offset > max_offset)
        offset = max_offset;

    return offset;
}

DglabListPage dglabListPageLayout(int view_top, int view_height, int content_height, int offset)
{
    DglabListPage page;

    page.view_top = view_top;
    page.view_height = view_height > 0 ? view_height : 0;
    page.content_height = content_height > 0 ? content_height : 0;
    page.max_offset = page.content_height > page.view_height
        ? page.content_height - page.view_height : 0;

    if (offset < 0)
        offset = 0;

    if (offset > page.max_offset)
        offset = page.max_offset;

    page.offset = offset;

    return page;
}

bool dglabListPageScrolls(const DglabListPage* page)
{
    return page->view_height > 0 && page->content_height > page->view_height;
}

void dglabListPageScrollBar(DglabCanvas* canvas, const DglabListPage* page)
{
    const DglabTheme* theme = dglabThemeGet();
    int thumb;
    int y;

    if (!dglabListPageScrolls(page))
        return;

    thumb = page->view_height * page->view_height / page->content_height;

    if (thumb < 40)
        thumb = 40;

    if (thumb > page->view_height)
        thumb = page->view_height;

    y = page->view_top + (page->view_height - thumb) * page->offset / page->max_offset;

    dglabCanvasFill(canvas, DGLAB_PAGE_WIDTH - 17, y, 4, thumb, theme->scrollbar);
}
