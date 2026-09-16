#include <dglab/ui/button.h>

#include <dglab/ui/theme.h>

#include <string.h>

// The console draws a button as a solid shape with its letter knocked out of it:
// in the screenshots an "A" is a hole in a filled disc, not a letter inside a
// thin ring (docs/nro-ui.md has the pixel map). The letter is therefore painted
// in the background colour - the icons only ever sit on the page background.
#define ICON_HEIGHT 26
#define FACE_RADIUS 13
#define BOX_RADIUS 6
#define SHOULDER_WIDTH 34

static uint32_t knockout(void)
{
    return dglabThemeGet()->background;
}

// The letter, centred in the box it is knocked out of.
static void letterInBox(DglabCanvas* canvas, DglabGlyphSource* font, const char* letter,
    int x, int y, int width, int height)
{
    int text_width = dglabTextWidth(font, letter);
    int text_y = y + (height - font->cell_height) / 2;

    dglabTextDraw(canvas, font, x + (width - text_width) / 2, text_y, letter, knockout());
}

// A filled triangle, the arrow inside the four direction buttons.
static void arrow(DglabCanvas* canvas, DglabButton button, int cx, int cy, int size,
    uint32_t color)
{
    for (int i = 0; i < size; i++) {
        int half = i / 2;

        switch (button) {
            case DglabButton_Up:
                dglabCanvasFill(canvas, cx - half, cy - size / 2 + i, half * 2 + 1, 1, color);
                break;
            case DglabButton_Down:
                dglabCanvasFill(canvas, cx - half, cy + size / 2 - i, half * 2 + 1, 1, color);
                break;
            case DglabButton_Left:
                dglabCanvasFill(canvas, cx - size / 2 + i, cy - half, 1, half * 2 + 1, color);
                break;
            default:
                dglabCanvasFill(canvas, cx + size / 2 - i, cy - half, 1, half * 2 + 1, color);
                break;
        }
    }
}

// Plus and minus, drawn rather than written: the font's glyphs sit on a text
// baseline and look off centre inside a box.
static void plusMinus(DglabCanvas* canvas, bool plus, int cx, int cy, int size, uint32_t color)
{
    int half = size / 2;

    dglabCanvasFill(canvas, cx - half, cy, size, 2, color);

    if (plus)
        dglabCanvasFill(canvas, cx, cy - half, 2, size, color);
}

void dglabButtonIcon(DglabCanvas* canvas, DglabGlyphSource* font, DglabButton button, int x,
    int y, uint32_t color)
{
    char label[3] = { 0, 0, 0 };
    int cx = x + ICON_HEIGHT / 2;
    int cy = y + ICON_HEIGHT / 2;

    switch (button) {
        case DglabButton_A: label[0] = 'A'; break;
        case DglabButton_B: label[0] = 'B'; break;
        case DglabButton_X: label[0] = 'X'; break;
        case DglabButton_Y: label[0] = 'Y'; break;
        case DglabButton_L: label[0] = 'L'; break;
        case DglabButton_R: label[0] = 'R'; break;
        case DglabButton_ZL: memcpy(label, "ZL", 2); break;
        case DglabButton_ZR: memcpy(label, "ZR", 2); break;
        default: break;
    }

    switch (button) {
        case DglabButton_A:
        case DglabButton_B:
        case DglabButton_X:
        case DglabButton_Y:
            dglabCanvasDisc(canvas, cx, cy, FACE_RADIUS, color);
            letterInBox(canvas, font, label, x, y, ICON_HEIGHT, ICON_HEIGHT);
            break;

        case DglabButton_L:
        case DglabButton_R:
            dglabCanvasRoundFill(canvas, x, y, ICON_HEIGHT, ICON_HEIGHT, BOX_RADIUS, color);
            letterInBox(canvas, font, label, x, y, ICON_HEIGHT, ICON_HEIGHT);
            break;

        case DglabButton_ZL:
        case DglabButton_ZR:
            dglabCanvasRoundFill(canvas, x, y, SHOULDER_WIDTH, ICON_HEIGHT, BOX_RADIUS, color);
            letterInBox(canvas, font, label, x, y, SHOULDER_WIDTH, ICON_HEIGHT);
            break;

        case DglabButton_Plus:
        case DglabButton_Minus:
            dglabCanvasRoundFill(canvas, x, y, ICON_HEIGHT, ICON_HEIGHT, BOX_RADIUS, color);
            plusMinus(canvas, button == DglabButton_Plus, cx, cy, 12, knockout());
            break;

        case DglabButton_DPad:
            // The cross as it looks from above, knocked out of the box.
            dglabCanvasRoundFill(canvas, x, y, ICON_HEIGHT, ICON_HEIGHT, BOX_RADIUS, color);
            dglabCanvasFill(canvas, cx - 1, cy - 7, 3, 15, knockout());
            dglabCanvasFill(canvas, cx - 7, cy - 1, 15, 3, knockout());
            break;

        default:
            dglabCanvasRoundFill(canvas, x, y, ICON_HEIGHT, ICON_HEIGHT, BOX_RADIUS, color);
            arrow(canvas, button, cx, cy, 11, knockout());
            break;
    }
}

int dglabButtonIconWidth(DglabButton button)
{
    if (button == DglabButton_ZL || button == DglabButton_ZR)
        return SHOULDER_WIDTH;

    return ICON_HEIGHT;
}
