#include <dglab/ui/canvas.h>

#include <string.h>

void dglabCanvasInit(DglabCanvas* canvas, uint8_t* pixels, int width, int height, int stride)
{
    canvas->pixels = pixels;
    canvas->width = width;
    canvas->height = height;
    canvas->stride = stride;

    dglabCanvasClearClip(canvas);
}

void dglabCanvasSetClip(DglabCanvas* canvas, int x, int y, int width, int height)
{
    if (!canvas)
        return;

    canvas->clip_x = x;
    canvas->clip_y = y;
    canvas->clip_width = width > 0 ? width : 0;
    canvas->clip_height = height > 0 ? height : 0;
}

void dglabCanvasClearClip(DglabCanvas* canvas)
{
    if (!canvas)
        return;

    dglabCanvasSetClip(canvas, 0, 0, canvas->width, canvas->height);
}

// Whether a pixel is inside the active clip rectangle. The canvas bounds are a
// separate check, because a clip set before dglabCanvasInit would have none.
static bool insideClip(const DglabCanvas* canvas, int x, int y)
{
    return x >= canvas->clip_x && y >= canvas->clip_y &&
           x < canvas->clip_x + canvas->clip_width &&
           y < canvas->clip_y + canvas->clip_height;
}

// Narrows a rectangle to the canvas and to the active clip. Returns false when
// nothing is left of it, which is the common case for a panel that a screen
// drew partly off the canvas.
static bool clipRect(const DglabCanvas* canvas, int* left, int* top, int* right, int* bottom)
{
    int clip_right = canvas->clip_x + canvas->clip_width;
    int clip_bottom = canvas->clip_y + canvas->clip_height;

    if (*left < 0)
        *left = 0;

    if (*top < 0)
        *top = 0;

    if (*right > canvas->width)
        *right = canvas->width;

    if (*bottom > canvas->height)
        *bottom = canvas->height;

    if (*left < canvas->clip_x)
        *left = canvas->clip_x;

    if (*top < canvas->clip_y)
        *top = canvas->clip_y;

    if (*right > clip_right)
        *right = clip_right;

    if (*bottom > clip_bottom)
        *bottom = clip_bottom;

    return *left < *right && *top < *bottom;
}

static void putPixel(DglabCanvas* canvas, int x, int y, uint32_t color)
{
    size_t offset = (size_t)y * (size_t)canvas->stride + (size_t)x * 4u;

    // The buffer is RGBA8888, so the bytes go out from the most significant one.
    canvas->pixels[offset + 0] = (uint8_t)(color >> 24);
    canvas->pixels[offset + 1] = (uint8_t)(color >> 16);
    canvas->pixels[offset + 2] = (uint8_t)(color >> 8);
    canvas->pixels[offset + 3] = (uint8_t)color;
}

void dglabCanvasBlend(DglabCanvas* canvas, int x, int y, uint32_t color, uint8_t alpha)
{
    uint8_t* pixel;
    int shift;

    if (!canvas->pixels || x < 0 || y < 0 || x >= canvas->width || y >= canvas->height)
        return;

    if (!insideClip(canvas, x, y))
        return;

    if (alpha == 0)
        return;

    pixel = (uint8_t*)canvas->pixels + (size_t)y * (size_t)canvas->stride + (size_t)x * 4;

    if (alpha == 255) {
        putPixel(canvas, x, y, color);
        return;
    }

    // Straight (non premultiplied) blend, one channel at a time.
    for (shift = 24; shift >= 0; shift -= 8) {
        uint32_t src = (color >> shift) & 0xFFu;

        pixel[(3 - (size_t)(shift / 8))] = (uint8_t)((src * alpha +
            (uint32_t)pixel[3 - (size_t)(shift / 8)] * (255u - alpha)) / 255u);
    }
}

void dglabCanvasFill(DglabCanvas* canvas, int x, int y, int width, int height, uint32_t color)
{
    if (!canvas->pixels || width <= 0 || height <= 0)
        return;

    int left = x;
    int top = y;
    int right = x + width;
    int bottom = y + height;

    if (!clipRect(canvas, &left, &top, &right, &bottom))
        return;

    for (int row = top; row < bottom; row++) {
        for (int col = left; col < right; col++)
            putPixel(canvas, col, row, color);
    }
}

void dglabCanvasFrame(DglabCanvas* canvas, int x, int y, int width, int height, int thickness,
    uint32_t color)
{
    if (thickness <= 0)
        return;

    if (thickness * 2 >= width || thickness * 2 >= height) {
        dglabCanvasFill(canvas, x, y, width, height, color);
        return;
    }

    dglabCanvasFill(canvas, x, y, width, thickness, color);
    dglabCanvasFill(canvas, x, y + height - thickness, width, thickness, color);
    dglabCanvasFill(canvas, x, y + thickness, thickness, height - thickness * 2, color);
    dglabCanvasFill(canvas, x + width - thickness, y + thickness, thickness,
        height - thickness * 2, color);
}

// How many of a pixel's four sample points fall inside the circle of `radius`
// (in pixels, the radius may be odd) centred on a pixel centre. The console's
// focus boxes and button rings are a handful of pixels across, and a hard edge
// at that size reads as a staircase, so the shapes are antialiased.
static int circleHits(int x, int y, int cx, int cy, int radius)
{
    int hits = 0;

    if (radius <= 0)
        return 0;

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            // Quarter pixel units, so the whole test stays in integers: the
            // samples sit at +0.25 and +0.75 of the pixel, the centre of pixel
            // cx is at 4 * cx + 2.
            int dx = x * 4 + 1 + i * 2 - (cx * 4 + 2);
            int dy = y * 4 + 1 + j * 2 - (cy * 4 + 2);
            int r = radius * 4;

            if (dx * dx + dy * dy <= r * r)
                hits++;
        }
    }

    return hits;
}

// How many of a pixel's four sample points fall inside the rounded rectangle,
// which is the same test with four quarter circles instead of one.
static int roundRectHits(int x, int y, int rx, int ry, int width, int height, int radius)
{
    int hits = 0;

    if (width <= 0 || height <= 0)
        return 0;

    if (radius * 4 > width * 2)
        radius = width / 2;

    if (radius * 4 > height * 2)
        radius = height / 2;

    if (radius < 0)
        radius = 0;

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int sx = (x - rx) * 4 + 1 + i * 2;
            int sy = (y - ry) * 4 + 1 + j * 2;
            int w = width * 4;
            int h = height * 4;
            int r = radius * 4;

            if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                continue;

            // The four corners are the only place a sample can fall outside.
            // The arc centres sit one radius in from each edge, so in quarter
            // pixel units they are at (r, r), (w - r, r) and so on.
            int cx = sx < r ? r : (sx >= w - r ? w - r : -1);
            int cy = sy < r ? r : (sy >= h - r ? h - r : -1);

            if (cx >= 0 && cy >= 0) {
                int dx = sx - cx;
                int dy = sy - cy;

                if (dx * dx + dy * dy >= r * r)
                    continue;
            }

            hits++;
        }
    }

    return hits;
}

// Blends one shape's coverage over whatever the pixel already holds.
static void blendHits(DglabCanvas* canvas, int x, int y, int hits, uint32_t color)
{
    if (hits <= 0)
        return;

    dglabCanvasBlend(canvas, x, y, color, (uint8_t)(hits * 255 / 4));
}

void dglabCanvasHLine(DglabCanvas* canvas, int x, int y, int width, uint32_t color)
{
    dglabCanvasFill(canvas, x, y, width, 1, color);
}

void dglabCanvasRoundFill(DglabCanvas* canvas, int x, int y, int width, int height, int radius,
    uint32_t color)
{
    if (width <= 0 || height <= 0)
        return;

    if (radius <= 0) {
        dglabCanvasFill(canvas, x, y, width, height, color);
        return;
    }

    if (radius * 2 > width)
        radius = width / 2;

    if (radius * 2 > height)
        radius = height / 2;

    // The straight middle is a plain fill; only the four corner squares need
    // the coverage test.
    dglabCanvasFill(canvas, x + radius, y, width - radius * 2, height, color);
    dglabCanvasFill(canvas, x, y + radius, radius, height - radius * 2, color);
    dglabCanvasFill(canvas, x + width - radius, y + radius, radius, height - radius * 2, color);

    for (int row = y; row < y + radius; row++) {
        int bottom = y + height - 1 - (row - y);

        for (int col = x; col < x + radius; col++) {
            int right = x + width - 1 - (col - x);

            blendHits(canvas, col, row,
                roundRectHits(col, row, x, y, width, height, radius), color);
            blendHits(canvas, right, row,
                roundRectHits(right, row, x, y, width, height, radius), color);
            blendHits(canvas, col, bottom,
                roundRectHits(col, bottom, x, y, width, height, radius), color);
            blendHits(canvas, right, bottom,
                roundRectHits(right, bottom, x, y, width, height, radius), color);
        }
    }
}

void dglabCanvasRoundFrame(DglabCanvas* canvas, int x, int y, int width, int height, int radius,
    int thickness, uint32_t color)
{
    int inner_x;
    int inner_y;
    int inner_width;
    int inner_height;
    int inner_radius;

    if (thickness <= 0 || width <= 0 || height <= 0)
        return;

    if (thickness * 2 >= width || thickness * 2 >= height) {
        dglabCanvasRoundFill(canvas, x, y, width, height, radius, color);
        return;
    }

    inner_x = x + thickness;
    inner_y = y + thickness;
    inner_width = width - thickness * 2;
    inner_height = height - thickness * 2;
    inner_radius = radius - thickness > 0 ? radius - thickness : 0;

    for (int row = y; row < y + height; row++) {
        for (int col = x; col < x + width; col++) {
            int outer = roundRectHits(col, row, x, y, width, height, radius);
            int inner = outer == 4
                ? roundRectHits(col, row, inner_x, inner_y, inner_width, inner_height, inner_radius)
                : 0;

            blendHits(canvas, col, row, outer - inner, color);
        }
    }
}

void dglabCanvasDisc(DglabCanvas* canvas, int cx, int cy, int radius, uint32_t color)
{
    if (radius <= 0)
        return;

    for (int row = cy - radius; row <= cy + radius; row++) {
        for (int col = cx - radius; col <= cx + radius; col++)
            blendHits(canvas, col, row, circleHits(col, row, cx, cy, radius), color);
    }
}

void dglabCanvasRing(DglabCanvas* canvas, int cx, int cy, int radius, int thickness,
    uint32_t color)
{
    int inner;

    if (radius <= 0 || thickness <= 0)
        return;

    inner = radius - thickness;

    for (int row = cy - radius; row <= cy + radius; row++) {
        for (int col = cx - radius; col <= cx + radius; col++) {
            int outer = circleHits(col, row, cx, cy, radius);
            int covered = outer == 4 ? outer - circleHits(col, row, cx, cy, inner) : outer;

            blendHits(canvas, col, row, covered, color);
        }
    }
}

static bool glyphPixel(const DglabFont* font, int glyph, int row, int col)
{
    int bytes_per_row = (font->tile_width + 7) / 8;
    size_t glyph_bytes = (size_t)bytes_per_row * (size_t)font->tile_height;
    const uint8_t* data = font->glyphs + (size_t)glyph * glyph_bytes + (size_t)row * bytes_per_row;

    // A row is a little endian value whose most significant bit is the leftmost
    // pixel, so column c is bit (tile_width - 1 - c) of the row. This matches
    // libnx's own console renderer, which walks the row's bits from the top.
    int bit = font->tile_width - 1 - col;

    return (data[bit / 8] >> (bit % 8)) & 1u;
}

void dglabCanvasText(DglabCanvas* canvas, const DglabFont* font, int x, int y, int scale,
    const char* text, uint32_t color)
{
    int cursor = x;

    if (scale <= 0)
        scale = 1;

    for (const char* p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        int glyph = (int)c - font->ascii_offset;

        if (glyph >= 0 && glyph < font->glyph_count) {
            for (int row = 0; row < font->tile_height; row++) {
                for (int col = 0; col < font->tile_width; col++) {
                    if (glyphPixel(font, glyph, row, col))
                        dglabCanvasFill(canvas, cursor + col * scale, y + row * scale, scale,
                            scale, color);
                }
            }
        }

        cursor += font->tile_width * scale;
    }
}

int dglabCanvasTextWidth(const DglabFont* font, int scale, const char* text)
{
    if (scale <= 0)
        scale = 1;

    return (int)strlen(text) * font->tile_width * scale;
}

void dglabCanvasQr(DglabCanvas* canvas, const DglabQrCode* code, int x, int y, int module_size,
    int quiet_zone, uint32_t dark, uint32_t light)
{
    int total;

    if (module_size <= 0)
        return;

    total = (int)code->size + quiet_zone * 2;

    if (light)
        dglabCanvasFill(canvas, x, y, total * module_size, total * module_size, light);

    for (int row = 0; row < code->size; row++) {
        for (int col = 0; col < code->size; col++) {
            if (!code->modules[row][col])
                continue;

            dglabCanvasFill(canvas, x + (col + quiet_zone) * module_size,
                y + (row + quiet_zone) * module_size, module_size, module_size, dark);
        }
    }
}
