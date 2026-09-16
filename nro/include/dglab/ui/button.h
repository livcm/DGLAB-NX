#pragma once

// The controller buttons, drawn as icons.
//
// The console's UI never spells a button's name out: the bottom bar shows the
// button's own icon next to the action it performs ("(B) 返回"). That is also
// why the button names are not in the string tables - an icon is not a
// translation - so this file is what turns a button into pixels.
//
// The console draws them solid with the letter knocked out of the shape (the
// screenshots in docs/nro-ui.md show an "A" as a hole in a filled disc), which
// is also what keeps the letter from crowding the outline.

#include <dglab/ui/canvas.h>
#include <dglab/ui/text.h>

typedef enum {
    DglabButton_A = 0,
    DglabButton_B,
    DglabButton_X,
    DglabButton_Y,
    DglabButton_L,
    DglabButton_R,
    DglabButton_ZL,
    DglabButton_ZR,
    DglabButton_Plus,
    DglabButton_Minus,
    DglabButton_Up,
    DglabButton_Down,
    DglabButton_Left,
    DglabButton_Right,
    DglabButton_DPad, ///< the whole cross, for "pick a row with up and down"
    DglabButton_Count,
    /// A hint with a single button: DglabHint's second slot carries this.
    DglabButton_None = DglabButton_Count,
} DglabButton;

/// Height of the box an icon occupies. The footer places all of them on this
/// line, whatever their own shape.
#define DGLAB_BUTTON_ICON_HEIGHT 26

/// Draws `button` with its top left corner at (x, y), filled with `color` and
/// with its letter or symbol knocked out in the page's background colour. `font`
/// draws that letter: DGLAB_TEXT_VALUE (22px) is the size the console uses.
void dglabButtonIcon(DglabCanvas* canvas, DglabGlyphSource* font, DglabButton button, int x,
    int y, uint32_t color);

/// How wide that icon is: the same box for the round buttons, wider for the
/// two letter shoulders.
int dglabButtonIconWidth(DglabButton button);

/// Space between the two icons of one hint (they are drawn a little apart so a
/// pair reads as two buttons and not as one wide one).
#define DGLAB_BUTTON_ICON_GAP 8
