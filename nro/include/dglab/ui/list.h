#pragma once

// The rows a page is made of, in the console's style: one rule between rows, the
// label on the left, the value on the right, and the focused row inside a ring.
//
// A screen builds its rows as an array and hands the same array to
// dglabListMeasure and dglabListDraw, so the height it scrolls by and the pixels
// it draws cannot disagree - the bug this project keeps hitting, where a panel
// was measured with one formula and filled with another.

#include <dglab/ui/canvas.h>
#include <dglab/ui/text.h>

#include <stdbool.h>

/// The height of one row, and of the ring drawn around a focused one. The ring
/// is taller than the row on purpose: the console's focus box overlaps the rules
/// above and below it (docs/nro-ui.md has the measurements).
#define DGLAB_ROW_HEIGHT 71
/// How far a row's text is inset from the rule that runs under it. The value on
/// the right keeps the same distance, so a screen that measures whether a value
/// fits has to subtract it twice.
#define DGLAB_ROW_PAD 16
#define DGLAB_ROW_FOCUS_HEIGHT 82
#define DGLAB_ROW_FOCUS_RADIUS 6
#define DGLAB_ROW_FOCUS_RING 3
/// The explanation font's line height, and the space a note block keeps around
/// itself.
#define DGLAB_NOTE_LINE 26
#define DGLAB_NOTE_GAP 24
/// A paragraph of body text (the console's white lead line), and its line pitch.
#define DGLAB_PARAGRAPH_LINE 34
#define DGLAB_PARAGRAPH_GAP 24

typedef enum {
    DglabRow_Item = 0, ///< label on the left, value on the right, focusable
    DglabRow_Note,     ///< a grey explanation, not focusable
    DglabRow_Paragraph,///< white body text, no bullet: the page's own words
} DglabRowKind;

typedef struct {
    DglabRowKind kind;
    const char* label; ///< Item/Slider: the text on the left
    const char* value; ///< Item: the text on the right, or NULL for none
    const char* note;  ///< optional explanation drawn under the row
    uint32_t value_color; ///< Item: how the value is painted
} DglabRow;

/// Where one row ended up, relative to the list's origin.
typedef struct {
    int y;
    int height;
} DglabRowBox;

typedef struct {
    DglabGlyphSource* body; ///< labels, values, sliders
    DglabGlyphSource* value;///< the (smaller) value font
    DglabGlyphSource* note; ///< explanations
} DglabListFonts;

typedef struct {
    int x;           ///< left edge of the rows (a page's column, or a panel's)
    int origin_y;    ///< where the list's first rule goes, after scrolling
    int width;       ///< the row width
    int focus;       ///< focused row, or -1 for a page with no focus
    bool navigation; ///< menu lists get the console's accent bar as well
} DglabListStyle;

/// The vertical layout of a page of rows: the band it is drawn in, how tall the
/// measured content is, and how far it can be scrolled.
///
/// Every page that draws a list builds this from the same DglabRow array it
/// hands to dglabListDraw, so the bar it shows and the rows it draws cannot
/// disagree - and "the content is taller than the page" is decided in one place
/// instead of once per screen (docs/nro-ui.md).
typedef struct {
    int view_top;       ///< the first y the content may use
    int view_height;    ///< how tall that band is
    int content_height; ///< what dglabListMeasure returned
    int max_offset;     ///< content_height - view_height, or 0
    int offset;         ///< the requested offset, clamped to what is reachable
} DglabListPage;

/// Measures every row into `boxes` and returns the total height. Returns 0 when
/// the rows do not fit `capacity`, which is a programming error rather than a
/// layout the caller has to handle: the screens size their row arrays statically.
int dglabListMeasure(const DglabListFonts* fonts, const DglabRow* rows, int count, int width,
    DglabRowBox* boxes, int capacity);

void dglabListDraw(DglabCanvas* canvas, const DglabListFonts* fonts, const DglabListStyle* style,
    const DglabRow* rows, const DglabRowBox* boxes, int count);

/// Moves the focus by `delta` rows, skipping the notes and wrapping around.
int dglabListFocusMove(const DglabRow* rows, int count, int focus, int delta);

/// Works out how tall the page's content is against the band it is drawn in, and
/// clamps `offset` to what is actually reachable (`0..max_offset`).
DglabListPage dglabListPageLayout(int view_top, int view_height, int content_height, int offset);

/// Whether the content is taller than the view: the page needs a way to scroll
/// it, and it is the same condition the scrollbar is drawn under.
bool dglabListPageScrolls(const DglabListPage* page);

/// The scrollbar the console draws on the right edge, when the content is taller
/// than the view. Drawn outside the content clip, and only then.
void dglabListPageScrollBar(DglabCanvas* canvas, const DglabListPage* page);

/// Keeps `y` (a row's own top, in content coordinates) inside the view, used to
/// scroll the focused row into sight.
int dglabListScrollFor(int offset, int max_offset, int view_height, int row_y, int row_height);

/// Keeps a scroll offset inside the content: 0 at the top, at most `max_offset`
/// at the bottom. A `max_offset` below 0 (a page that fits) means 0.
int dglabListScrollClamp(int offset, int max_offset);
