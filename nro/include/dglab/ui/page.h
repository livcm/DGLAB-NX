#pragma once

// The frame every screen sits in: a title bar with one white rule under it, the
// content column, and a bottom bar with the button hints. Measured off the
// console's own UI at 720p - docs/nro-ui.md keeps the table.
//
// A page has no border and no panel: the console separates things with single
// pixel rules and space, which is the whole difference from the framed panels
// this UI used before.

#include <dglab/ui/button.h>
#include <dglab/ui/canvas.h>
#include <dglab/ui/text.h>

#define DGLAB_PAGE_WIDTH 1280
#define DGLAB_PAGE_HEIGHT 720
/// Left and right margin of the two rules that split the page.
#define DGLAB_PAGE_MARGIN 24
/// The title is indented past the rule, the way the console indents it.
#define DGLAB_PAGE_TITLE_X 72
/// The rule under the title bar (y = 87, one pixel) and the one above the bottom
/// bar (y = 647): the header is 0..88 and the bottom bar 647..720. Both are
/// white - the console reserves #4D4D4D for the rules between content rows.
#define DGLAB_PAGE_RULE_Y 87
#define DGLAB_PAGE_BAR_Y 647
/// The content column of a page with one column of rows: x=220..1060, which is
/// how the console insets a page of prose or of full width rows.
#define DGLAB_PAGE_CONTENT_X 220
#define DGLAB_PAGE_CONTENT_WIDTH 840
/// The band a page with two columns uses instead. The console gives that layout
/// much smaller outer margins - the left column starts at 80 and the right one
/// runs to 1189, i.e. 91 pixels short of the screen edge (docs/nro-ui.md has the
/// measurements from both reference pages).
#define DGLAB_PAGE_WIDE_X 80
#define DGLAB_PAGE_WIDE_WIDTH 1110
#define DGLAB_PAGE_CONTENT_TOP 128
/// The last y content may reach, so nothing runs under the bottom bar.
#define DGLAB_PAGE_CONTENT_BOTTOM DGLAB_PAGE_BAR_Y
/// Where the content clip starts: one pixel under the title rule, so the focus
/// ring of the first row - which is taller than its row and reaches ~6px above
/// it - is drawn whole instead of being cut off at the top.
#define DGLAB_PAGE_CLIP_TOP (DGLAB_PAGE_RULE_Y + 1)
/// The bottom bar's hints are laid out from the right edge inwards.
#define DGLAB_PAGE_HINTS_RIGHT 1216
#define DGLAB_PAGE_HINT_ICON_Y 672
/// Space between the icon and its action, and between two hints.
#define DGLAB_PAGE_HINT_TEXT_GAP 12
#define DGLAB_PAGE_HINT_GAP 43

/// One entry in the bottom bar: one or two buttons and what they do here. The
/// button's name is its icon, so `action` is only the action ("返回") and comes
/// from the string tables; "B" never does. A pair (ZL+ZR, left+right) is drawn
/// as two icons followed by one action.
typedef struct {
    DglabButton button;
    DglabButton button2; ///< DglabButton_None for a single button hint
    const char* action;
} DglabHint;

/// Fills the page and draws the two rules.
void dglabPageBegin(DglabCanvas* canvas);

/// The page title (28px), plus an optional right hand side - the link state, the
/// IPC version - drawn in its own style and right aligned with the hints. Pass a
/// NULL `right` or `right_text` for a page without one.
void dglabPageHeader(DglabCanvas* canvas, const DglabTextStyle* title, const char* title_text,
    const DglabTextStyle* right, const char* right_text);

/// The bottom bar's hints, in reading order (left to right).
void dglabPageHints(DglabCanvas* canvas, DglabGlyphSource* font, const DglabHint* hints,
    int count);

/// How wide one hint is (its buttons, the gap, and the action).
int dglabHintWidth(DglabGlyphSource* font, const DglabHint* hint);

/// Draws one hint with its top left corner at (x, y). Used by the bottom bar and
/// by the screens that put a hint line inside the content ("上/下 调整 A").
void dglabHintDraw(DglabCanvas* canvas, DglabGlyphSource* font, const DglabHint* hint, int x,
    int y, uint32_t color);

/// Confines drawing to the content column between the title rule and the bottom
/// bar. Rows scrolled past the edge are cut there instead of over the rules.
void dglabPageClipContent(DglabCanvas* canvas);

/// The same, for a page laid out in two columns: the band is wider because the
/// console's two column pages use smaller outer margins.
void dglabPageClipWide(DglabCanvas* canvas);
