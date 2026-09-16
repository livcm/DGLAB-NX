#include <dglab/platform/framebuffer.h>

#include <switch.h>

#define DGLAB_FB_WIDTH 1280
#define DGLAB_FB_HEIGHT 720
// What a docked console draws instead: the same layout, 1.5x the pixels, so the
// TV gets a native 1080p picture rather than a scaled 720p one.
#define DGLAB_FB_DOCK_WIDTH 1920
#define DGLAB_FB_DOCK_HEIGHT 1080
#define DGLAB_FB_DOCK_SCALE_NUM 3
#define DGLAB_FB_DOCK_SCALE_DEN 2

// libnx's default console font. There is no public declaration for it in the
// headers, so it is declared here on purpose: the symbol exists in libnx.a
// (verified with nm, size 0x2000 = 256 glyphs of 32 bytes) and console.h
// documents it as the default font with 16x16 tiles. The bit layout of those
// tiles was verified by extracting the font data and reading glyphs out of it.
extern const u8 default_font_bin[];

static Framebuffer g_framebuffer;
static bool g_open;
static DglabFont g_font;
static u32 g_width = DGLAB_FB_WIDTH;
static u32 g_height = DGLAB_FB_HEIGHT;
static int g_scale_num = 1;
static int g_scale_den = 1;

// One attempt at one resolution. nwindowSetDimensions has to happen while the
// window has no buffers registered, which is why the retry below runs before the
// framebuffer exists rather than after a failure.
static bool openAt(u32 width, u32 height, int scale_num, int scale_den)
{
    NWindow* window = nwindowGetDefault();

    if (R_FAILED(nwindowSetDimensions(window, width, height)))
        return false;

    if (R_FAILED(framebufferCreate(&g_framebuffer, window, width, height,
            PIXEL_FORMAT_RGBA_8888, 2)))
        return false;

    if (R_FAILED(framebufferMakeLinear(&g_framebuffer))) {
        framebufferClose(&g_framebuffer);
        return false;
    }

    g_font.glyphs = default_font_bin;
    g_font.ascii_offset = 0;
    g_font.glyph_count = 256;
    g_font.tile_width = 16;
    g_font.tile_height = 16;

    g_width = width;
    g_height = height;
    g_scale_num = scale_num;
    g_scale_den = scale_den;
    g_open = true;

    return true;
}

bool dglabFramebufferOpen(void)
{
    if (g_open)
        return true;

    if (appletGetOperationMode() == AppletOperationMode_Console &&
        openAt(DGLAB_FB_DOCK_WIDTH, DGLAB_FB_DOCK_HEIGHT, DGLAB_FB_DOCK_SCALE_NUM,
            DGLAB_FB_DOCK_SCALE_DEN))
        return true;

    // Handheld, or a docked console that refused the 1080p frame: the console's
    // own scaler takes a 720p frame up to the TV.
    return openAt(DGLAB_FB_WIDTH, DGLAB_FB_HEIGHT, 1, 1);
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

    dglabCanvasInit(canvas, (uint8_t*)base, (int)g_width, (int)g_height, (int)stride);
    dglabCanvasSetScale(canvas, g_scale_num, g_scale_den);

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

void dglabFramebufferScale(int* num, int* den)
{
    if (num)
        *num = g_scale_num;

    if (den)
        *den = g_scale_den;
}
