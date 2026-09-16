#include <dglab/ui/canvas.h>

#include <string.h>

void dglabCanvasInit(DglabCanvas* canvas, uint8_t* pixels, int width, int height, int stride)
{
    canvas->pixels = pixels;
    canvas->width = width;
    canvas->height = height;
    canvas->stride = stride;
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

    if (left < 0)
        left = 0;

    if (top < 0)
        top = 0;

    if (right > canvas->width)
        right = canvas->width;

    if (bottom > canvas->height)
        bottom = canvas->height;

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
