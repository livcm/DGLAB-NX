#include <dglab/platform/framebuffer.h>

#include <switch.h>

#define DGLAB_FB_WIDTH 1280
#define DGLAB_FB_HEIGHT 720

// libnx's default console font. There is no public declaration for it in the
// headers, so it is declared here on purpose: the symbol exists in libnx.a
// (verified with nm, size 0x2000 = 256 glyphs of 32 bytes) and console.h
// documents it as the default font with 16x16 tiles. The bit layout of those
// tiles was verified by extracting the font data and reading glyphs out of it.
extern const u8 default_font_bin[];

static Framebuffer g_framebuffer;
static bool g_open;
static DglabFont g_font;

bool dglabFramebufferOpen(void)
{
    framebufferCreate(&g_framebuffer, nwindowGetDefault(), DGLAB_FB_WIDTH, DGLAB_FB_HEIGHT,
        PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&g_framebuffer);

    g_font.glyphs = default_font_bin;
    g_font.ascii_offset = 0;
    g_font.glyph_count = 256;
    g_font.tile_width = 16;
    g_font.tile_height = 16;

    g_open = true;

    return true;
}

void dglabFramebufferClose(void)
{
    if (!g_open)
        return;

    framebufferClose(&g_framebuffer);
    g_open = false;
}

bool dglabFramebufferBegin(DglabCanvas* canvas)
{
    u32 stride = 0;
    void* base;

    if (!g_open)
        return false;

    base = framebufferBegin(&g_framebuffer, &stride);

    if (base == NULL)
        return false;

    dglabCanvasInit(canvas, (uint8_t*)base, DGLAB_FB_WIDTH, DGLAB_FB_HEIGHT, (int)stride);

    return true;
}

void dglabFramebufferEnd(void)
{
    framebufferEnd(&g_framebuffer);
}

const DglabFont* dglabFramebufferFont(void)
{
    return &g_font;
}
