// Host side tests for the NRO canvas.
//
// The canvas is what the framebuffer backend and (later) a deko3d backend draw
// through, so the clipping and the bitmap font layout are checked here rather
// than on the console.
//
// Run with: make -C tests/canvas

#include <dglab/ui/canvas.h>
#include <dglab/ui/about.h>
#include <dglab/ui/advanced.h>
#include <dglab/ui/button.h>
#include <dglab/ui/list.h>
#include <dglab/ui/menu.h>
#include <dglab/ui/motion.h>
#include <dglab/ui/touch.h>
#include <dglab/ui/page.h>
#include <dglab/ui/screen.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>
#include <dglab/nro/motion_settings.h>

#include <stdio.h>
#include <string.h>

#include "../lang/lang_fixture.h"

static int g_checks;
static int g_failures;

// The screen metrics and the page helpers are defined further down, next to the
// block font they are built on; the early tests use them (the motion page is
// rendered to look for ink in a band, and the menu one to count what it drew).
static DglabFontSet blockFonts(void);
static uint32_t screenPixel(int x, int y);
static size_t screenBytes(void);
static void beginPage(DglabCanvas* canvas, uint32_t background);
static void setScreenSize(int width, int height, int scale_num, int scale_den);

// Counts the pixels a screen changed away from the background it was filled
// with, which is how these tests notice a screen that drew nothing at all.
static int countChangedPixels(const uint8_t* pixels, size_t size, uint32_t background)
{
    int changed = 0;

    for (size_t i = 0; i + 3 < size; i += 4) {
        uint32_t pixel = ((uint32_t)pixels[i] << 24) | ((uint32_t)pixels[i + 1] << 16) |
                         ((uint32_t)pixels[i + 2] << 8) | pixels[i + 3];

        if (pixel != background)
            changed++;
    }

    return changed;
}

#define CHECK(condition)                                                \
    do {                                                                \
        g_checks++;                                                     \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

// How many pixels two frames of the same page differ in. A test that cannot read
// the face (the block font has one glyph) can still say "this value reached the
// drawing" by changing the value and watching the frame move.
static int countDifferingPixels(const uint8_t* a, const uint8_t* b, size_t size)
{
    int different = 0;

    for (size_t i = 0; i + 3 < size; i += 4) {
        if (memcmp(a + i, b + i, 4) != 0)
            different++;
    }

    return different;
}

#define TEST_WIDTH 64
#define TEST_HEIGHT 32

// An 8x8 font with one glyph ('A' at index 65) that has single pixels at (0,0)
// and (3,3). A row's most significant bit is its leftmost pixel, which is the
// layout libnx's font uses, so column 0 is bit 7 and column 3 is bit 4.
static const uint8_t kGlyph[8] = { 0x80, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00 };

static const DglabFont kFont = {
    .glyphs = kGlyph,
    .ascii_offset = 65,
    .glyph_count = 1,
    .tile_width = 8,
    .tile_height = 8,
};

static uint8_t g_pixels[TEST_WIDTH * TEST_HEIGHT * 4];

static void clear(uint32_t color)
{
    DglabCanvas canvas;

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);
    dglabCanvasFill(&canvas, 0, 0, TEST_WIDTH, TEST_HEIGHT, color);
}

static uint32_t pixelAt(int x, int y)
{
    size_t offset = ((size_t)y * TEST_WIDTH + (size_t)x) * 4u;

    return ((uint32_t)g_pixels[offset] << 24) | ((uint32_t)g_pixels[offset + 1] << 16) |
           ((uint32_t)g_pixels[offset + 2] << 8) | (uint32_t)g_pixels[offset + 3];
}

#define SCREEN_PIXEL_WIDTH 1280
#define SCREEN_PIXEL_HEIGHT 720
// The docked frame is the same layout at 1.5x (docs/nro-ui.md), so the pages are
// rendered at both sizes into the same buffer.
#define DOCK_PIXEL_WIDTH 1920
#define DOCK_PIXEL_HEIGHT 1080
#define DOCK_SCALE_NUM 3
#define DOCK_SCALE_DEN 2

static uint8_t g_screen_pixels[DOCK_PIXEL_WIDTH * DOCK_PIXEL_HEIGHT * 4];
static int g_screen_width = SCREEN_PIXEL_WIDTH;
static int g_screen_height = SCREEN_PIXEL_HEIGHT;
static int g_scale_num = 1;
static int g_scale_den = 1;
// The canvas the page under test is being drawn on, for the region checks: they
// ask it to convert the logical bounds of a region, so they cannot drift from
// what the drawing calls do.
static DglabCanvas g_page_canvas;

static void testFillAndClip(void)
{
    const uint32_t red = DGLAB_RGBA(0xFF, 0, 0, 0xFF);
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabCanvas canvas;

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);

    clear(blue);
    dglabCanvasFill(&canvas, 4, 4, 8, 8, red);
    CHECK(pixelAt(4, 4) == red);
    CHECK(pixelAt(11, 11) == red);
    CHECK(pixelAt(12, 12) == blue);
    CHECK(pixelAt(3, 3) == blue);

    // Partially off screen: only the visible part changes, nothing else.
    clear(blue);
    dglabCanvasFill(&canvas, -4, -4, 10, 10, red);
    CHECK(pixelAt(0, 0) == red);
    CHECK(pixelAt(5, 5) == red);
    CHECK(pixelAt(6, 6) == blue);

    // Entirely off screen.
    clear(blue);
    dglabCanvasFill(&canvas, TEST_WIDTH + 4, 0, 8, 8, red);
    dglabCanvasFill(&canvas, 0, -100, 8, 8, red);
    CHECK(pixelAt(0, 0) == blue);
    CHECK(pixelAt(TEST_WIDTH - 1, TEST_HEIGHT - 1) == blue);
}

static void testFrame(void)
{
    const uint32_t red = DGLAB_RGBA(0xFF, 0, 0, 0xFF);
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabCanvas canvas;

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);

    clear(blue);
    dglabCanvasFrame(&canvas, 2, 2, 10, 10, 2, red);

    CHECK(pixelAt(2, 2) == red);     // corner
    CHECK(pixelAt(11, 2) == red);    // top right
    CHECK(pixelAt(2, 11) == red);    // bottom left
    CHECK(pixelAt(6, 6) == blue);    // inside stays untouched
    CHECK(pixelAt(12, 12) == blue);  // outside stays untouched
}

static void testText(void)
{
    const uint32_t white = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabCanvas canvas;

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);

    CHECK(dglabCanvasTextWidth(&kFont, 1, "AAA") == 24);
    CHECK(dglabCanvasTextWidth(&kFont, 2, "AA") == 32);

    clear(blue);
    dglabCanvasText(&canvas, &kFont, 1, 1, 1, "A", white);
    CHECK(pixelAt(1, 1) == white);
    CHECK(pixelAt(4, 4) == white);
    CHECK(pixelAt(2, 2) == blue);
    // A glyph must not come out mirrored: the pixel that belongs to column 0
    // has to land on the left edge, and the right edge stays empty.
    CHECK(pixelAt(8, 1) == blue);

    // Scaled text covers a block of pixels per font pixel.
    clear(blue);
    dglabCanvasText(&canvas, &kFont, 0, 0, 3, "A", white);
    CHECK(pixelAt(0, 0) == white);
    CHECK(pixelAt(2, 2) == white);
    // The font pixel at (3,3) covers 9..11 at this scale.
    CHECK(pixelAt(9, 9) == white);
    CHECK(pixelAt(11, 11) == white);
    CHECK(pixelAt(12, 12) == blue);

    // Characters outside the font are skipped but still advance the cursor.
    clear(blue);
    dglabCanvasText(&canvas, &kFont, 0, 0, 1, "?A", white);
    CHECK(pixelAt(0, 0) == blue);
    CHECK(pixelAt(8, 0) == white);
}

static void testQr(void)
{
    const uint32_t dark = DGLAB_RGBA(0, 0, 0, 0xFF);
    const uint32_t light = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabQrCode code;
    DglabCanvas canvas;

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);

    CHECK(dglabQrEncodeString(&code, "hello, dg-lab!", DglabQrEcc_M));

    clear(blue);
    dglabCanvasQr(&canvas, &code, 2, 2, 1, 1, dark, light);

    // Quiet zone around the symbol is filled with the light colour.
    CHECK(pixelAt(2, 2) == light);
    CHECK(pixelAt(2 + code.size, 2 + code.size) == light);

    // The finder pattern's top left module is dark, and it starts one module
    // after the quiet zone.
    CHECK(code.modules[0][0] == 1);
    CHECK(pixelAt(3, 3) == dark);

    // Nothing outside the requested area changed.
    CHECK(pixelAt(0, 0) == blue);
    CHECK(pixelAt(1, 1) == blue);
}

// The layout must stay inside the canvas it is given, whatever the state says.
static void testScreen(void)
{
    static uint8_t screen_pixels[1280 * 720 * 4];
    // A full log panel: the newest line has to stay inside the panel too.
    static const char* log_lines[DGLAB_SCREEN_LOG_LINES] = {
        "listening on port 9999", "app bound", "socket server core ready",
        "accept from 10.0.0.9", "websocket from 10.0.0.9, target '/8f2a4c1e'",
        "app 3c71d0b2-4e55-4a9f-9f1b-2b3c4d5e6f70 bound", "tx heartbeat", "rx ping #1",
        "waveform ch A, 48 slots", "tx clear-A", "tx waveform ch A, 32 slots", "tx waveform ch A, 16 slots",
    };
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabScreenState state;
    DglabCanvas canvas;
    int changed = 0;

    dglabCanvasInit(&canvas, screen_pixels, 1280, 720, 1280 * 4);
    dglabCanvasFill(&canvas, 0, 0, 1280, 720, blue);

    // The socket page's right column carries a row as "label left, value right",
    // so the longest pair has to fit inside the column: the id the page shortens
    // and the address the sysmodule reports are the two that come closest.
    {
        int inside = DGLAB_SOCKET_INFO_WIDTH - 2 * 16;

        CHECK(dglabCanvasTextWidth(&kFont, 1, "address") + 24 +
                  dglabCanvasTextWidth(&kFont, 1, "192.168.1.161:9999") <=
              inside);
        // The uuid is shown whole now that the column is wide; the shortened
        // form is the fallback the screen measures for itself.
        CHECK(dglabCanvasTextWidth(&kFont, 1, "app id") + 24 +
                  dglabCanvasTextWidth(&kFont, 1, "3c71d0b2-4e55-4a9f-9f1b-2b3c4d5e6f70") <=
              inside);
        CHECK(dglabCanvasTextWidth(&kFont, 1, "app id") + 24 +
                  dglabCanvasTextWidth(&kFont, 1, "3c71d0b2...4d5e6f70") <=
              inside);
        CHECK(dglabCanvasTextWidth(&kFont, 1, "last cmd") + 24 +
                  dglabCanvasTextWidth(&kFont, 1, "waveform A  no app bound") <=
              inside);
    }

    // A row is the label on the left and the value on the right, so what has to
    // fit is both of them plus a gap between: the widest row this state can
    // produce is the one whose value main.c builds for a command.
    {
        int labels = dglabCanvasTextWidth(&kFont, 1, "app report");
        int inside = DGLAB_PAGE_CONTENT_WIDTH;

        CHECK(labels + 24 + dglabCanvasTextWidth(&kFont, 1, "375/100") <= inside);
        CHECK(labels + 24 + dglabCanvasTextWidth(&kFont, 1, "A test  ok (A is 0)") <= inside);
        CHECK(dglabCanvasTextWidth(&kFont, 1, "8f2a4c1e...5e6f7a8b9c0d") +
                  dglabCanvasTextWidth(&kFont, 1, "app id") + 24 <=
              inside);
    }

    memset(&state, 0, sizeof(state));
    state.status_ok = true;
    state.sysmodule_ok = true;
    state.status.state = DglabNetState_Paired;
    state.status.port = 9999;
    state.status.app_feedback = DGLAB_NET_FEEDBACK_NONE;
    snprintf((char*)state.status.ip_text, sizeof(state.status.ip_text), "10.0.0.2");
    snprintf((char*)state.status.controller_id, DGLAB_NET_ID_LEN,
        "8f2a4c1e-9b77-4d21-8c3a-5e6f7a8b9c0d");
    snprintf((char*)state.status.peer_id, DGLAB_NET_ID_LEN,
        "3c71d0b2-4e55-4a9f-9f1b-2b3c4d5e6f70");
    state.url = "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#"
                "ws://10.0.0.2:9999/8f2a4c1e-9b77-4d21-8c3a-5e6f7a8b9c0d";
    state.url_ok = true;
    // Both channels at the widest the value column has to hold.
    state.test_strength_a = 100;
    state.test_strength_b = 100;
    state.last_command = "A test  ok (A is 0)";
    state.last_command_tone = DglabCmdTone_Warn;
    state.log_lines = log_lines;
    state.log_count = DGLAB_SCREEN_LOG_LINES;

    {
        DglabFontSet fonts = blockFonts();

        dglabScreenDraw(&canvas, &fonts, &state);
    }

    changed = countChangedPixels(screen_pixels, sizeof(screen_pixels), blue);

    // The chrome fills most of the screen, but the corners keep the background
    // and the code must not have run off the end of the buffer.
    CHECK(changed > 1280 * 720 / 2);
}

// The mode menu: every entry draws, and the selection stops at the ends instead
// of wrapping around (the list is a list; stepping off the bottom used to jump
// back to the top, which reads as a lost keypress).
static void testMenu(void)
{
    static uint8_t screen_pixels[1280 * 720 * 4];
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabFontSet fonts = blockFonts();
    DglabMenuState menu;
    DglabCanvas canvas;

    CHECK(dglabMenuMove(0, -1) == 0);
    CHECK(dglabMenuMove(0, 1) == 1);
    CHECK(dglabMenuMove(DglabMenu_ItemCount - 1, 1) == DglabMenu_ItemCount - 1);
    CHECK(dglabMenuMove(DglabMenu_ItemCount - 1, -1) == DglabMenu_ItemCount - 2);
    CHECK(dglabMenuMove(1, -(int)DglabMenu_ItemCount) == 0);
    CHECK(dglabMenuMove(1, (int)DglabMenu_ItemCount) == DglabMenu_ItemCount - 1);

    for (unsigned item = 0; item < (unsigned)DglabMenu_ItemCount; item++) {
        int changed;

        CHECK(dglabMenuItemName(item)[0] != '\0');
        CHECK(dglabMenuItemDescription(item)[0] != '\0');
        CHECK(dglabMenuMove(item, 0) == item);

        memset(&menu, 0, sizeof(menu));
        menu.selected = item;
        menu.sysmodule_ok = (item % 2) == 0;

        dglabCanvasInit(&canvas, screen_pixels, 1280, 720, 1280 * 4);
        dglabCanvasFill(&canvas, 0, 0, 1280, 720, blue);
        dglabMenuDraw(&canvas, &fonts, &menu);

        changed = countChangedPixels(screen_pixels, sizeof(screen_pixels), blue);
        CHECK(changed > 1280 * 720 / 2);
    }
}

// The motion screen: the widest values it can hold still have to fit, and it
// draws for both a connected and a missing Joy-Con.
static void testMotionScreen(void)
{
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabMotionScreenState state;
    DglabCanvas canvas;
    int changed;

    memset(&state, 0, sizeof(state));
    state.left_connected = true;
    state.right_connected = false;
    state.moving_a = true;
    state.level_a = 100;
    state.frequency_a = 30;
    state.frequency_b = 100;
    state.channel_strength_a = 100;
    state.channel_strength_b = 0;
    state.link = "app connected";
    state.link_tone = DglabCmdTone_Ok;
    state.last_upload = "waveform A  ok";
    state.last_upload_tone = DglabCmdTone_Ok;
    state.server_running = true;

    // Same rule as the socket page: label on the left, value on the right, and
    // the longest of each has to fit the content column together. The labels are
    // the localised strings, so this covers the Chinese table too.
    {
        int inside = DGLAB_PAGE_CONTENT_WIDTH;
        int widest = dglabCanvasTextWidth(&kFont, 1, "moving   waveform 100   100ms");
        char strength_label[64];
        int labels;

        // The two label shapes the rows use: the strength rows suffix the local
        // name with " A" / " B", the input rows are the localised Joy-Con names.
        snprintf(strength_label, sizeof(strength_label), "%s A",
            dglabString(DglabString_MotionVolume));

        labels = dglabCanvasTextWidth(&kFont, 1, strength_label);
        {
            int joycon = dglabCanvasTextWidth(&kFont, 1,
                dglabString(DglabString_MotionJoyConLeft));

            if (joycon > labels)
                labels = joycon;
        }

        CHECK(labels + 24 + widest <= inside);
    }

    setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);
    beginPage(&canvas, blue);
    {
        DglabFontSet fonts = blockFonts();

        dglabMotionScreenDraw(&canvas, &fonts, &state);
    }

    changed = countChangedPixels(g_screen_pixels, screenBytes(), blue);
    CHECK(changed > 1280 * 720 / 2);

    // The page has no grey explanation under its last row any more: the note that
    // used to sit there ("the channel strength is the volume ... set on the
    // socket server page") restated what the rows above it already show, and the
    // strength is edited on this very page. A note leaves ink in the band between
    // the last row and the bottom bar, and the rows themselves stop well above
    // it, so ink there means the note came back.
    {
        int top = DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + 8 + 5 * DGLAB_ROW_HEIGHT + 20;
        uint32_t page_background = dglabThemeGet()->background;
        int found = 0;

        for (int y = top; y < DGLAB_PAGE_BAR_Y; y++) {
            for (int x = DGLAB_PAGE_CONTENT_X;
                 x < DGLAB_PAGE_CONTENT_X + DGLAB_PAGE_CONTENT_WIDTH; x++) {
                uint32_t pixel = screenPixel(x, y);

                if (pixel != blue && pixel != page_background)
                    found++;
            }
        }

        CHECK(found == 0);
    }
}

// The advanced screen: every setting has a name and a description, and the
// longest name still leaves room for a value on the same row.
//
// The table is the same one the screen draws from: the localized name of each
// setting, so both language files are covered by the width check below.
static const DglabString kSettingNameKeys[DglabMotionSetting_Count] = {
    [DglabMotionSetting_DeadzoneEnter] = DglabString_SetDeadzoneEnter,
    [DglabMotionSetting_DeadzoneExit] = DglabString_SetDeadzoneExit,
    [DglabMotionSetting_GyroRange] = DglabString_SetGyroRange,
    [DglabMotionSetting_AccelRange] = DglabString_SetAccelRange,
    [DglabMotionSetting_GyroWeight] = DglabString_SetGyroWeight,
    [DglabMotionSetting_AccelWeight] = DglabString_SetAccelWeight,
    [DglabMotionSetting_Attack] = DglabString_SetAttack,
    [DglabMotionSetting_Release] = DglabString_SetRelease,
    [DglabMotionSetting_IdleStop] = DglabString_SetIdleStop,
    [DglabMotionSetting_FrequencyFast] = DglabString_SetFrequencyFast,
    [DglabMotionSetting_FrequencyStill] = DglabString_SetFrequencyStill,
    [DglabMotionSetting_StrengthMax] = DglabString_SetStrengthMax,
};

static void testAdvancedScreen(void)
{
    static uint8_t screen_pixels[1280 * 720 * 4];
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabMotionFeedConfig config;
    DglabAdvancedState state;
    DglabCanvas canvas;
    int changed;

    for (unsigned setting = 0; setting < (unsigned)DglabMotionSetting_Count; setting++) {
        CHECK(dglabMotionSettingName(setting)[0] != '\0');
        CHECK(dglabMotionSettingDescription(setting)[0] != '\0');
        CHECK(dglabCanvasTextWidth(&kFont, 1, dglabString(kSettingNameKeys[setting])) + 24 +
                  dglabCanvasTextWidth(&kFont, 1, "1000ms") <=
              DGLAB_PAGE_CONTENT_WIDTH);
    }

    dglabMotionSettingsDefault(&config);
    dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFast, -4);

    memset(&state, 0, sizeof(state));
    state.config = &config;
    state.selected = DglabMotionSetting_FrequencyFast;
    state.saved = true;

    dglabCanvasInit(&canvas, screen_pixels, 1280, 720, 1280 * 4);
    dglabCanvasFill(&canvas, 0, 0, 1280, 720, blue);
    {
        DglabFontSet fonts = blockFonts();

        dglabAdvancedDraw(&canvas, &fonts, &state);
    }

    changed = countChangedPixels(screen_pixels, sizeof(screen_pixels), blue);
    CHECK(changed > 1280 * 720 / 2);
}

// The blend the antialiased text needs: coverage must mix, not overwrite.
static void testBlend(void)
{
    const uint32_t black = DGLAB_RGBA(0, 0, 0, 0xFF);
    const uint32_t white = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
    DglabCanvas canvas;

    memset(g_pixels, 0, sizeof(g_pixels));
    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);
    dglabCanvasFill(&canvas, 0, 0, 16, 16, black);

    dglabCanvasBlend(&canvas, 4, 4, white, 0);
    CHECK(pixelAt(4, 4) == black);

    dglabCanvasBlend(&canvas, 4, 4, white, 255);
    CHECK(pixelAt(4, 4) == white);

    dglabCanvasFill(&canvas, 4, 4, 1, 1, black);
    dglabCanvasBlend(&canvas, 4, 4, white, 128);
    {
        uint32_t mixed = pixelAt(4, 4);
        unsigned level = (mixed >> 24) & 0xFFu;

        CHECK(level >= 126 && level <= 130);
    }

    // Out of bounds is ignored, like every other canvas call.
    dglabCanvasBlend(&canvas, -1, 4, white, 128);
    dglabCanvasBlend(&canvas, TEST_WIDTH, 4, white, 128);
}

// Every page, with the longest state each one can be given: the socket page with
// a bound app and a full log, the advanced page on its last setting, and the menu
// on every entry in turn. What is checked is where the ink ended up, not what it
// says.
//
// `frames` is what the run is called in a failure message, `fonts` the sizes the
// pages draw with: the same suite runs for the handheld frame and for the docked
// one, whose glyphs are rasterised 1.5x larger while the layout stays in logical
// units (docs/nro-ui.md).
// ---------------------------------------------------------------------------
// The screens at the console's metrics
// ---------------------------------------------------------------------------
//
// The screens were laid out against libnx's 16px bitmap font, and on the
// console the 24px system font is wider and taller than that: a row that fits
// here was half a row too tall there and the drawing ended up on a panel
// border. This glyph source has the console's metrics (24px cells, a 34px line)
// and a solid block for every character, so any string that overflows its box
// is a full block of pixels where it should not be.

// The blocks have the console font's advances: half a line for ASCII, a whole
// one for everything else (a Chinese character is a full em wide, a Latin
// letter is about half of that).
#define BLOCK_SIZE 24
#define BLOCK_ASCII_WIDTH 12
#define BLOCK_LINE_HEIGHT 34
#define BLOCK_ASCENT 28
#define BLOCK_CELL_HEIGHT 35

static uint8_t g_block_bitmap[BLOCK_SIZE * BLOCK_SIZE];

static bool blockLookup(DglabGlyphSource* source, uint32_t codepoint, DglabGlyph* out)
{
    (void)source;

    memset(out, 0, sizeof(*out));
    out->pixels = g_block_bitmap;
    out->coverage = true;
    out->stride = BLOCK_SIZE;
    out->width = codepoint < 0x80 ? BLOCK_ASCII_WIDTH : BLOCK_SIZE;
    out->height = BLOCK_SIZE;
    out->bearing_x = 0;
    out->bearing_y = BLOCK_ASCENT;
    out->advance = out->width;

    return true;
}

static DglabGlyphSource* blockSource(void)
{
    static DglabGlyphSource source;

    memset(g_block_bitmap, 0xFF, sizeof(g_block_bitmap));

    source.lookup = blockLookup;
    source.line_height = BLOCK_LINE_HEIGHT;
    source.ascent = BLOCK_ASCENT;
    source.cell_height = BLOCK_CELL_HEIGHT;
    source.context = NULL;

    return &source;
}

// Wrapping has to cut a line only when the text really goes on: the English
// sleep warning used to come out as three lines ("… press" / "Y" / "first")
// because every line ended at the last space the wrapper had seen, even when
// the whole rest of the text still fitted on the line. The console's font is
// proportional and this one is not, so the widths here are picked to put the
// break in the same place the screenshot showed.
static void testWrap(void)
{
    DglabGlyphSource* source = blockSource();
    const char* warning = "do not sleep while the server runs: press Y first";
    char line[192];
    size_t taken;

    // Text that fits is one line, not one line per word: ASCII is 12 pixels
    // wide in this source, so 480 pixels hold 40 characters.
    taken = dglabTextWrapLine(source, "short line", 480, line, sizeof(line));
    CHECK(taken == strlen("short line"));
    CHECK(strcmp(line, "short line") == 0);

    // The warning is one line up to "press" (42 characters with the space) and
    // then exactly one more: "Y" alone is the bug this test is here for.
    CHECK(dglabTextCountLines(source, warning, 505) == 2);

    taken = dglabTextWrapLine(source, warning, 505, line, sizeof(line));
    CHECK(taken == strlen("do not sleep while the server runs: press"));
    CHECK(strcmp(line, "do not sleep while the server runs: press") == 0);

    taken = dglabTextWrapLine(source, warning + taken + 1, 505, line, sizeof(line));
    CHECK(taken == strlen("Y first"));
    CHECK(strcmp(line, "Y first") == 0);

    // A word wider than the line is cut rather than allowed to overflow.
    taken = dglabTextWrapLine(source, "aaaaaaaaaaaaaaa", 48, line, sizeof(line));
    CHECK(taken == 4);
    CHECK(strcmp(line, "aaaa") == 0);

    // Chinese has no spaces, so it breaks between characters (24 pixels each).
    taken = dglabTextWrapLine(source, "中文中文", 48, line, sizeof(line));
    CHECK(taken == strlen("中文"));
    CHECK(strcmp(line, "中文") == 0);

    // The space a line breaks at belongs to neither line.
    {
        const char* text = "one two three";

        taken = dglabTextWrapLine(source, text, 12 * 8, line, sizeof(line));
        CHECK(taken == strlen("one two"));
        CHECK(strcmp(line, "one two") == 0);
        CHECK(text[taken] == ' ');
    }
}

// The sizes a screen draws with. The host has no system font, so every size is
// the block font: what these tests check is where the layout puts things, and
// that does not depend on the face.
static DglabFontSet blockFonts(void)
{
    DglabFontSet fonts;

    fonts.title = blockSource();
    fonts.body = fonts.title;
    fonts.value = fonts.title;
    fonts.note = fonts.title;
    fonts.icon = fonts.title;

    return fonts;
}

// The same, for the docked frame: the logical metrics are unchanged (that is
// what keeps the layout identical) while the bitmaps come out 1.5x larger, the
// way dglabTtfFontCreate() rasterises them at a 3/2 display scale. A glyph box
// that is not scaled the same way as the pen is what would make text drift, so
// the pages are rendered with this source as well.
#define DOCK_BLOCK_SIZE 36
#define DOCK_BLOCK_ASCII_WIDTH 18

static uint8_t g_dock_bitmap[DOCK_BLOCK_SIZE * DOCK_BLOCK_SIZE];

static bool dockBlockLookup(DglabGlyphSource* source, uint32_t codepoint, DglabGlyph* out)
{
    (void)source;

    memset(out, 0, sizeof(*out));
    out->pixels = g_dock_bitmap;
    out->coverage = true;
    out->stride = DOCK_BLOCK_SIZE;
    out->width = codepoint < 0x80 ? DOCK_BLOCK_ASCII_WIDTH : DOCK_BLOCK_SIZE;
    out->height = DOCK_BLOCK_SIZE;
    out->bearing_x = 0;
    out->bearing_y = BLOCK_ASCENT;
    out->advance = codepoint < 0x80 ? BLOCK_ASCII_WIDTH : BLOCK_SIZE;

    return true;
}

static DglabGlyphSource* dockBlockSource(void)
{
    static DglabGlyphSource source;

    memset(g_dock_bitmap, 0xFF, sizeof(g_dock_bitmap));

    source.lookup = dockBlockLookup;
    source.line_height = BLOCK_LINE_HEIGHT;
    source.ascent = BLOCK_ASCENT;
    source.cell_height = BLOCK_CELL_HEIGHT;
    source.context = NULL;

    return &source;
}

static DglabFontSet dockFonts(void)
{
    DglabFontSet fonts;
    DglabGlyphSource* source = dockBlockSource();

    fonts.title = source;
    fonts.body = source;
    fonts.value = source;
    fonts.note = source;
    fonts.icon = source;

    return fonts;
}

static uint32_t screenPixel(int x, int y)
{
    size_t offset = ((size_t)y * (size_t)g_screen_width + (size_t)x) * 4u;

    return ((uint32_t)g_screen_pixels[offset] << 24) |
           ((uint32_t)g_screen_pixels[offset + 1] << 16) |
           ((uint32_t)g_screen_pixels[offset + 2] << 8) | (uint32_t)g_screen_pixels[offset + 3];
}

// How much of the screen buffer is in use: the buffer itself is sized for the
// largest frame these tests render.
static size_t screenBytes(void)
{
    return (size_t)g_screen_width * (size_t)g_screen_height * 4u;
}

static void beginPage(DglabCanvas* canvas, uint32_t background)
{
    dglabCanvasInit(canvas, g_screen_pixels, g_screen_width, g_screen_height, g_screen_width * 4);
    dglabCanvasSetScale(canvas, g_scale_num, g_scale_den);
    dglabCanvasFill(canvas, 0, 0, SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, background);
    g_page_canvas = *canvas;
}

// Switches the whole page suite between the handheld frame and the docked one.
static void setScreenSize(int width, int height, int scale_num, int scale_den)
{
    g_screen_width = width;
    g_screen_height = height;
    g_scale_num = scale_num;
    g_scale_den = scale_den;
}

// Where a page may draw: the header band, the content column, the bottom bar, and
// the scrollbar's own column on the right. A pixel outside all four is a layout
// that grew past what it was measured for.
//
// This is the console page's version of "nothing on the frame": the old screens
// were checked for text against a panel's border, and there is no border any
// more, so what has to hold is that nothing leaves the column it belongs to.
// The one pixel of slack absorbs the antialiased edge of the focus ring.
//
// The bounds are logical and go through the canvas, which is drawing the page:
// the same check then holds for the docked 1080p frame, whose pixels are the
// logical ones 1.5x larger.
// What a page is allowed to paint. The console's own pages are one column of
// rows, or - for the two column socket page - a wider band; the touch mode is the
// first page whose content *is* the band between the two rules, so it declares
// that instead of a column (nro/AGENTS.md).
typedef enum {
    PageRegion_Rows = 0,
    PageRegion_Wide,
    PageRegion_Playfield,
} PageRegion;

static bool pageRegion(int x, int y, PageRegion region)
{
    const DglabCanvas* canvas = &g_page_canvas;

    if (y <= dglabCanvasScale(canvas, DGLAB_PAGE_RULE_Y))
        return true;

    if (y >= dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y))
        return true;

    if (x >= dglabCanvasScale(canvas, DGLAB_PAGE_WIDTH - 20))
        return true;

    // A page whose content is the whole band: the touch mode's field reaches the
    // screen edges between the rules, because the field is what the user is
    // aiming at. It is confined to that band all the same - the title bar, the
    // bottom bar and the page margin outside the rules are still off limits,
    // which is what this check is here for.
    if (region == PageRegion_Playfield)
        return y >= dglabCanvasScale(canvas, DGLAB_PAGE_CLIP_TOP - 1) &&
               y < dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y);

    // A one column page keeps to x=220..1060; a two column page uses the wider
    // band, because that is what the console does (docs/nro-ui.md). The clip
    // starts one pixel under the title rule, so a focused first row's ring is
    // drawn whole.
    if (region == PageRegion_Wide && x >= dglabCanvasScale(canvas, DGLAB_PAGE_WIDE_X - 1) &&
        x <= dglabCanvasScale(canvas, DGLAB_PAGE_WIDE_X + DGLAB_PAGE_WIDE_WIDTH))
        return y >= dglabCanvasScale(canvas, DGLAB_PAGE_CLIP_TOP - 1) &&
               y < dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_BOTTOM);

    return x >= dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_X - 1) &&
           x <= dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_X + DGLAB_PAGE_CONTENT_WIDTH) &&
           y >= dglabCanvasScale(canvas, DGLAB_PAGE_CLIP_TOP - 1) &&
           y < dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_BOTTOM);
}

static void checkPageStaysInItsRegions(const char* name, uint32_t background, PageRegion region)
{
    // The page fills itself with the theme's own background, so both colours are
    // "nothing was drawn here": what the check looks for is ink.
    uint32_t page_background = dglabThemeGet()->background;
    int outside = 0;
    int drawn = 0;

    for (int y = 0; y < g_screen_height; y++) {
        for (int x = 0; x < g_screen_width; x++) {
            uint32_t pixel = screenPixel(x, y);

            if (pixel == background || pixel == page_background)
                continue;

            drawn++;

            if (pageRegion(x, y, region))
                continue;

            if (outside < 8)
                printf("    %s: %d,%d is %08X, outside the page's regions\n", name, x, y,
                    screenPixel(x, y));

            outside++;
        }
    }

    if (outside)
        printf("  %s: %d pixels outside the page's regions\n", name, outside);

    CHECK(outside == 0);
    // A page that drew nothing at all would pass the check above: the header
    // rule, the title and the bottom bar alone are a few thousand pixels.
    CHECK(drawn > 2000);

    // Both rules are white and one pixel tall. The margin beside them is left
    // alone as well - the console reserves the grey #4D4D4D for the rows inside
    // the page (docs/nro-ui.md) - except on a page whose own content is the band,
    // where a line at the first column of the rules is the point: the touch mode
    // draws the density axis' ends there, on purpose.
    CHECK(screenPixel(dglabCanvasScale(&g_page_canvas, 640),
              dglabCanvasScale(&g_page_canvas, DGLAB_PAGE_RULE_Y)) == dglabThemeGet()->rule);
    CHECK(screenPixel(dglabCanvasScale(&g_page_canvas, 640),
              dglabCanvasScale(&g_page_canvas, DGLAB_PAGE_BAR_Y)) == dglabThemeGet()->rule);

    if (region != PageRegion_Playfield) {
        CHECK(screenPixel(dglabCanvasScale(&g_page_canvas, 24),
                  dglabCanvasScale(&g_page_canvas, DGLAB_PAGE_BAR_Y) - 3) == page_background);
    }

    CHECK(screenPixel(dglabCanvasScale(&g_page_canvas, 24),
              dglabCanvasScale(&g_page_canvas, DGLAB_PAGE_BAR_Y) + 3) == page_background);
}

// The console draws a button as a solid shape with its letter knocked out, not
// as a thin ring. On the host's block font that is a direct check: the fill is
// the icon colour, the middle of the shape is the background (the hole the
// letter leaves) and the corner outside the disc is background again.
static void testButtonIcons(void)
{
    const uint32_t icon = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
    DglabTheme theme = dglabThemeDark;
    DglabGlyphSource* font = blockSource();
    DglabCanvas canvas;

    dglabThemeSet(&theme);

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);
    dglabCanvasFill(&canvas, 0, 0, TEST_WIDTH, TEST_HEIGHT, theme.background);

    dglabButtonIcon(&canvas, font, DglabButton_A, 0, 0, icon);

    CHECK(pixelAt(1, 1) == theme.background);   // the box corner outside the disc
    CHECK(pixelAt(1, 13) == icon);              // the disc's own fill
    CHECK(pixelAt(13, 13) == theme.background); // the knocked out letter
    CHECK(pixelAt(0, 0) == theme.background);   // the corner outside the circle

    // The two shoulders are wider than a face button, so a ZL+ZR hint reads as
    // two icons rather than as one wide one.
    CHECK(dglabButtonIconWidth(DglabButton_ZL) > dglabButtonIconWidth(DglabButton_A));

    dglabThemeSet(NULL);
}

// A glyph source whose ink is a solid rectangle: the shape of one capital letter
// of a font of that size. The button icon is drawn with DGLAB_TEXT_ICON (20px),
// whose capitals are about 14px tall, and the letter has to sit inside the 26px
// shape without touching the outline.
#define INK_MAX 64
#define INK_FONTS 4

typedef struct {
    DglabGlyphSource source;
    uint8_t bitmap[INK_MAX * INK_MAX];
    int width;
    int height;
    int ascent;
} InkFont;

static bool inkLookup(DglabGlyphSource* source, uint32_t codepoint, DglabGlyph* out)
{
    const InkFont* font = source->context;

    (void)codepoint;

    memset(out, 0, sizeof(*out));
    out->pixels = font->bitmap;
    out->coverage = true;
    out->stride = font->width;
    out->width = font->width;
    out->height = font->height;
    out->bearing_x = 0;
    out->bearing_y = font->ascent;
    out->advance = font->width;

    return true;
}

static DglabGlyphSource* inkSource(int width, int height, int ascent)
{
    static InkFont fonts[INK_FONTS];
    static unsigned used;
    InkFont* font = &fonts[used++ % INK_FONTS];

    font->width = width;
    font->height = height;
    font->ascent = ascent;
    memset(font->bitmap, 0xFF, sizeof(font->bitmap));

    font->source.lookup = inkLookup;
    font->source.ascent = ascent;
    font->source.cell_height = ascent + height / 4;
    font->source.line_height = ascent + height / 4;
    font->source.context = font;

    return &font->source;
}

// The first and last row of the letter inside one of the icon's columns: a run of
// background pixels inside the shape, which is what the knocked out letter is.
static void letterRows(int column, int* first, int* last)
{
    uint32_t page_background = dglabThemeGet()->background;

    *first = -1;
    *last = -1;

    for (int y = 0; y < DGLAB_BUTTON_ICON_HEIGHT; y++) {
        if (pixelAt(column, y) != page_background)
            continue;

        if (*first < 0)
            *first = y;

        *last = y;
    }
}

// The letter keeps its distance from the outline. Drawing it with the row sized
// font, centred on the line box - which is what the icons used to do - put its
// ink 1px from the edge of the disc, and that is what the user's screenshot of
// the footer shows.
static void testButtonIconLetterMargins(void)
{
    const uint32_t icon = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
    DglabTheme theme = dglabThemeDark;
    DglabGlyphSource* font = inkSource(14, 14, 22);
    int ink_height = dglabTextInkHeight(font, "A");
    DglabCanvas canvas;
    int first;
    int last;

    dglabThemeSet(&theme);

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);
    dglabCanvasFill(&canvas, 0, 0, TEST_WIDTH, TEST_HEIGHT, theme.background);

    // A round face button, measured down its middle: the disc is filled, so the
    // only background in that column is the letter.
    dglabButtonIcon(&canvas, font, DglabButton_A, 0, 0, icon);

    letterRows(13, &first, &last);

    CHECK(first >= 5);
    CHECK(last <= DGLAB_BUTTON_ICON_HEIGHT - 6);
    CHECK(last - first + 1 == ink_height);

    // And the same in one of the boxed shapes, which the letters of L and ZL sit
    // in. The box is a rounded rectangle, so its middle column is solid too.
    dglabCanvasFill(&canvas, 0, 0, TEST_WIDTH, TEST_HEIGHT, theme.background);
    dglabButtonIcon(&canvas, font, DglabButton_L, 0, 0, icon);

    letterRows(13, &first, &last);

    CHECK(first >= 5);
    CHECK(last <= DGLAB_BUTTON_ICON_HEIGHT - 6);

    dglabThemeSet(NULL);
}

// The action text of a hint is drawn in whatever font the page passes, but the
// button icon is always drawn in the icon font: the two are separate arguments,
// and this is what holds them apart.
static void testHintIconUsesTheIconFont(void)
{
    const uint32_t icon = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
    DglabTheme theme = dglabThemeDark;
    DglabGlyphSource* icon_font = inkSource(12, 12, 18);
    DglabGlyphSource* text_font = inkSource(24, 30, 34);
    DglabHint hint = { DglabButton_L, DglabButton_None, "ab" };
    DglabCanvas canvas;
    int first;
    int last;

    dglabThemeSet(&theme);

    dglabCanvasInit(&canvas, g_pixels, TEST_WIDTH, TEST_HEIGHT, TEST_WIDTH * 4);
    dglabCanvasFill(&canvas, 0, 0, TEST_WIDTH, TEST_HEIGHT, theme.background);

    dglabHintDraw(&canvas, icon_font, text_font, &hint, 0, 0, icon);

    letterRows(13, &first, &last);

    // The hole is the icon font's letter, not the 30px block the action text
    // would leave if the two were swapped.
    CHECK(last - first + 1 == 12);

    dglabThemeSet(NULL);
}

// The first row's focus ring is taller than its row, so it reaches above the
// content column's own top edge. The clip starts under the title rule, which is
// what keeps the top of that ring from being cut off (the bug this checks).
static void testFirstRowFocusRingIsWhole(void)
{
    DglabFontSet fonts = blockFonts();
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabMenuState menu;
    DglabCanvas canvas;
    int ring = 0;

    memset(&menu, 0, sizeof(menu));
    menu.selected = 0;
    menu.sysmodule_ok = true;

    beginPage(&canvas, blue);
    dglabMenuDraw(&canvas, &fonts, &menu);

    // Anything drawn between the title rule and the first row can only be that
    // ring: the rows themselves start at the content top.
    for (int y = DGLAB_PAGE_RULE_Y + 1; y < DGLAB_PAGE_CONTENT_TOP; y++) {
        for (int x = DGLAB_PAGE_CONTENT_X; x < DGLAB_PAGE_CONTENT_X + DGLAB_PAGE_CONTENT_WIDTH;
             x++) {
            if (screenPixel(x, y) == dglabThemeGet()->focus_ring)
                ring++;
        }
    }

    CHECK(ring > 100);
}

// The pages with no cursor do not scroll, so their content has to end above the
// bottom bar on its own: ink in the last few pixels is a column that was cut off
// rather than laid out. (The pages that scroll - the menu, the advanced
// parameters, the log - are allowed to run up to the clip edge.)
static void checkContentClearsTheBar(const char* name)
{
    uint32_t background = dglabThemeGet()->background;
    int found = 0;
    const DglabCanvas* canvas = &g_page_canvas;
    // The last few logical pixels above the bar, which is where a column that was
    // cut off rather than laid out leaves ink.
    int top = dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y - 6);
    int bottom = dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y);
    int left = dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_X);
    int right = dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_X + DGLAB_PAGE_CONTENT_WIDTH);

    for (int y = top; y < bottom; y++) {
        for (int x = left; x < right; x++) {
            if (screenPixel(x, y) == background)
                continue;

            if (found < 4)
                printf("    %s: %d,%d is content in the last pixels above the bar\n", name, x,
                    y);

            found++;
        }
    }

    if (found)
        printf("  %s: %d pixels of content run into the bottom bar\n", name, found);

    CHECK(found == 0);
}

// The touch page's field runs from rule to rule on purpose - it *is* the input -
// so the band above the bottom bar holds its own ink (the centre line, the tick
// marks, the marker) and the check above cannot read it the way it reads a page
// of rows. What still has to hold is the one row that can overflow, the note a
// docked console gets, so the field's own fixed geometry is masked out instead of
// the check being dropped: anything else in the last pixels above the bar is a
// row that did not fit.
static void checkFieldClearsTheBar(const char* name)
{
    // The vertical lines the field draws, in buffer pixels: the centre line and
    // the two lines that close the density axis, plus the three ticks of each
    // half - all from the drawing code's own formulas.
    const uint32_t probes[2] = { 0u, (uint32_t)DGLAB_TOUCH_SPLIT };
    uint32_t background = dglabThemeGet()->background;
    const DglabCanvas* canvas = &g_page_canvas;
    int lines[3 + 2 * 3];
    int line_count = 0;
    int top = dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y - 6);
    int bottom = dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y);
    int left = dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_X);
    int right = dglabCanvasScale(canvas, DGLAB_PAGE_CONTENT_X + DGLAB_PAGE_CONTENT_WIDTH);
    int found = 0;

    lines[line_count++] = dglabCanvasScale(canvas, DGLAB_TOUCH_SPLIT);
    lines[line_count++] = dglabCanvasScale(canvas, DGLAB_TOUCH_DENSITY_LEFT);
    lines[line_count++] = dglabCanvasScale(canvas, DGLAB_TOUCH_DENSITY_RIGHT);

    for (int half = 0; half < 2; half++) {
        uint32_t start = dglabTouchDensityStart(probes[half]);
        int density_span = (int)(dglabTouchDensityEnd(probes[half]) - start);

        for (int step = 1; step < 4; step++) {
            int tick = (int)start + density_span * step / 4;

            lines[line_count++] = dglabCanvasScale(canvas, tick);
        }
    }

    for (int y = top; y < bottom; y++) {
        for (int x = left; x < right; x++) {
            bool masked = false;

            for (int i = 0; i < line_count; i++) {
                if (x >= lines[i] - 2 && x <= lines[i] + 2)
                    masked = true;
            }

            if (masked || screenPixel(x, y) == background)
                continue;

            if (found < 4)
                printf("    %s: %d,%d is content in the last pixels above the bar\n", name, x,
                    y);

            found++;
        }
    }

    if (found)
        printf("  %s: %d pixels of content run into the bottom bar\n", name, found);

    CHECK(found == 0);
}

// Nothing the field draws goes past the density axis: the horizontal quarter lines
// stop at the two vertical lines that close it, and the finger markers are clamped
// into the same box, so a finger in a clamp band - or on the page header, or below
// the bottom rule - shows just inside the edge it went past. The strips outside the
// axis are exactly where a reading is clamped to the end of the range, so ink there
// would describe a position nobody can dial in.
static void checkFieldStaysInsideTheAxis(const char* name)
{
    const DglabCanvas* canvas = &g_page_canvas;
    uint32_t background = dglabThemeGet()->background;
    int value_span = DGLAB_TOUCH_VALUE_BOTTOM - DGLAB_TOUCH_VALUE_TOP;
    int outside_left = dglabCanvasScale(canvas, DGLAB_TOUCH_DENSITY_LEFT - 1);
    int outside_right = dglabCanvasScale(canvas, DGLAB_TOUCH_DENSITY_RIGHT + 1);
    int inside = dglabCanvasScale(canvas,
        (DGLAB_TOUCH_DENSITY_LEFT + DGLAB_TOUCH_DENSITY_RIGHT) / 2);
    int band_top = dglabCanvasScale(canvas, DGLAB_PAGE_CLIP_TOP - 1);
    int band_bottom = dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y);
    int strips[2][2] = {
        { 0, outside_left },
        { outside_right, g_screen_width },
    };
    int outside = 0;

    for (int strip = 0; strip < 2; strip++) {
        for (int y = band_top; y < band_bottom; y++) {
            for (int x = strips[strip][0]; x < strips[strip][1]; x++) {
                if (screenPixel(x, y) == background)
                    continue;

                if (outside < 4)
                    printf("    %s: %d,%d is ink outside the density axis\n", name, x, y);

                outside++;
            }
        }
    }

    if (outside)
        printf("  %s: %d pixels outside the density axis\n", name, outside);

    CHECK(outside == 0);

    for (int step = 1; step < 4; step++) {
        int y = dglabCanvasScale(canvas, DGLAB_TOUCH_VALUE_TOP + value_span * step / 4);

        CHECK(screenPixel(outside_left, y) == background);
        CHECK(screenPixel(outside_right, y) == background);
        // And the line itself is still there, between the two ends.
        CHECK(screenPixel(inside, y) != background);
    }
}

// A docked console gets no field at all: no grid, no centre line, no axis ends,
// no markers - the panel is inside the dock, there is nothing to point at, and
// lines nobody can use would only look like they could be used. What is left in
// the band under the page's rows is the one line that says so, and it has to be
// centred: a stray grid line, or a line that was not centred, moves the middle of
// that ink away from the middle of the panel and this fails.
static void checkDockedField(const char* name)
{
    const DglabCanvas* canvas = &g_page_canvas;
    uint32_t background = dglabThemeGet()->background;
    // Under the five rows the page always lays out, from the same origin the
    // screens use (dglabListMeasure + the list style's own offset).
    int top = dglabCanvasScale(canvas,
        DGLAB_PAGE_CONTENT_TOP + DGLAB_NOTE_LINE + 8 + DGLAB_ROW_HEIGHT * 5 + 8);
    int bottom = dglabCanvasScale(canvas, DGLAB_PAGE_BAR_Y);
    int centre = dglabCanvasScale(canvas, DGLAB_TOUCH_PANEL_WIDTH / 2);
    int min_x = -1;
    int max_x = -1;
    int ink = 0;

    for (int y = top; y < bottom; y++) {
        for (int x = 0; x < g_screen_width; x++) {
            if (screenPixel(x, y) == background)
                continue;

            ink++;

            if (min_x < 0 || x < min_x)
                min_x = x;

            if (x > max_x)
                max_x = x;
        }
    }

    if (ink == 0)
        printf("  %s: the docked page does not say why there is nothing to touch\n", name);

    CHECK(ink > 0);
    CHECK(min_x >= 0 && (min_x + max_x) / 2 > centre - 8 && (min_x + max_x) / 2 < centre + 8);
}

// A QR code is only worth showing while a socket is listening behind it. The
// payload reaches the page as soon as the console has a LAN address - NET_QR
// answers then, because the address on its own is useful - and the page used to
// paint the code anyway, which is the "the server is not even started and the
// code is already there" the user saw.
static void testQrOnlyWhileTheServerRuns(void)
{
    static const char* const url =
        "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#"
        "ws://192.168.1.161:9999/8f2a4c1e-9b77-4d21-8c3a-5e6f7a8b9c0d";
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabFontSet fonts = blockFonts();
    DglabScreenState state;
    DglabCanvas canvas;

    setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);

    // The code is the only thing on this page painted pure black, and it is
    // always in the code's own column.
    for (unsigned variant = 0; variant < 2; variant++) {
        bool running = variant != 0;
        int black = 0;

        memset(&state, 0, sizeof(state));
        state.status_ok = true;
        state.status.state = running ? DglabNetState_Listening : DglabNetState_Idle;
        state.status.port = 9999;
        snprintf((char*)state.status.ip_text, sizeof(state.status.ip_text), "192.168.1.161");
        snprintf((char*)state.status.controller_id, DGLAB_NET_ID_LEN,
            "8f2a4c1e-9b77-4d21-8c3a-5e6f7a8b9c0d");
        state.url = url;
        state.url_ok = true;

        beginPage(&canvas, blue);
        dglabScreenDraw(&canvas, &fonts, &state);

        for (int y = DGLAB_PAGE_CONTENT_TOP; y < DGLAB_PAGE_CONTENT_BOTTOM; y++) {
            for (int x = DGLAB_SOCKET_QR_X; x < DGLAB_SOCKET_QR_X + DGLAB_SOCKET_QR_WIDTH;
                 x++) {
                if (screenPixel(x, y) == dglabThemeGet()->black)
                    black++;
            }
        }

        if (running)
            CHECK(black > 1000);
        else
            CHECK(black == 0);
    }
}

// The docked frame draws the same layout 1.5x larger. Two things have to hold for
// that to be invisible: rectangles that share an edge still share it (a seam
// would run along every rule and every row), and a glyph whose bitmap was
// rasterised 1.5x larger still lands where the logical metrics put it.
static void testScaledCanvas(void)
{
    const uint32_t red = DGLAB_RGBA(0xFF, 0, 0, 0xFF);
    const uint32_t blue = DGLAB_RGBA(0, 0, 0, 0xFF);
    const uint32_t text = DGLAB_RGBA(0xFF, 0xFF, 0xFF, 0xFF);
    DglabFontSet fonts = dockFonts();
    DglabCanvas canvas;
    int gap = 0;

    setScreenSize(DOCK_PIXEL_WIDTH, DOCK_PIXEL_HEIGHT, DOCK_SCALE_NUM, DOCK_SCALE_DEN);
    beginPage(&canvas, blue);

    // Two 100x100 logical rectangles side by side.
    dglabCanvasFill(&canvas, 100, 100, 100, 100, red);
    dglabCanvasFill(&canvas, 200, 100, 100, 100, red);

    for (int y = dglabCanvasScale(&canvas, 100); y < dglabCanvasScale(&canvas, 200); y++) {
        for (int x = dglabCanvasScale(&canvas, 100); x < dglabCanvasScale(&canvas, 300); x++) {
            if (screenPixel(x, y) != red)
                gap++;
        }
    }

    CHECK(gap == 0);
    CHECK(screenPixel(dglabCanvasScale(&canvas, 300), dglabCanvasScale(&canvas, 150)) == blue);

    // A rule is one logical pixel; at this scale it must cover at least the one
    // buffer pixel it started as, and never less.
    dglabCanvasHLine(&canvas, 100, 400, 100, red);
    CHECK(screenPixel(dglabCanvasScale(&canvas, 150), dglabCanvasScale(&canvas, 400)) == red);

    // The metrics a screen reads stay logical at this scale, which is what keeps
    // the layout identical to the handheld one.
    CHECK(dglabTextWidth(fonts.body, "中") == BLOCK_SIZE);
    CHECK(dglabTextWidth(fonts.body, "AB") == 2 * BLOCK_ASCII_WIDTH);

    // One CJK block covers its whole 24 logical pixel cell, so at this scale its
    // ink has to cover 36 buffer pixels starting at the pen.
    dglabCanvasFill(&canvas, 0, 0, SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, blue);
    dglabTextDraw(&canvas, fonts.body, 300, 300, "中", text);

    {
        int left = dglabCanvasScale(&canvas, 300);
        int top = dglabCanvasScale(&canvas, 300);

        CHECK(screenPixel(left, top) == text);
        CHECK(screenPixel(left + DOCK_BLOCK_SIZE - 1, top + DOCK_BLOCK_SIZE - 1) == text);
        CHECK(screenPixel(left + DOCK_BLOCK_SIZE, top) == blue);
    }

    setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);
}

static void checkEveryPage(const char* frames, const DglabFontSet* fonts)
{
    static const char* log_lines[DGLAB_SCREEN_LOG_LINES] = {
        "sleep watch unavailable rc=0x000108C3",
        "socket server core ready, controller id 8f2a4c1e-9b77",
        "websocket from 172.20.10.2, target '/8f2a4c1e'",
        "listening on port 9999",
        "app bound",
        "socket server core ready",
        "accept from 10.0.0.9",
        "websocket from 10.0.0.9, target '/8f2a4c1e'",
        "app 3c71d0b2-4e55-4a9f-9f1b-2b3c4d5e6f70 bound",
        "tx heartbeat",
        "rx ping #1",
        "waveform ch A, 48 slots",
    };
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabMotionFeedConfig config;
    DglabCanvas canvas;

    dglabMotionSettingsDefault(&config);

    {
        // The menu, on every entry: the description under the selected one is
        // what changes the page's height.
        for (unsigned item = 0; item < (unsigned)DglabMenu_ItemCount; item++) {
            DglabMenuState menu;
            char name[96];

            memset(&menu, 0, sizeof(menu));
            menu.selected = item;
            menu.sysmodule_ok = (item % 2) == 0;

            snprintf(name, sizeof(name), "%s menu %u", frames, item);
            beginPage(&canvas, blue);
            dglabMenuDraw(&canvas, fonts, &menu);
            checkPageStaysInItsRegions(name, blue, PageRegion_Rows);
        }

        // The socket page, in the states that change what it draws: the server
        // running or not, a QR code available or not, and - while the server runs
        // - whether automatic sleep is suppressed, which is the other wording of
        // the warning line under the server row. The log page is part of the same
        // view.
        for (unsigned variant = 0; variant < 6; variant++) {
            bool running = (variant & 1) != 0;
            bool have_qr = (variant & 2) != 0;
            bool suppressed = running && (variant & 4) != 0;
            DglabScreenState screen;
            char name[96];

            memset(&screen, 0, sizeof(screen));
            screen.status_ok = true;
            screen.sysmodule_ok = (variant % 2) == 0;
            screen.status.state = running ? DglabNetState_Paired : DglabNetState_Idle;
            screen.status.port = 9999;
            screen.status.sessions = 9999;
            screen.status.commands_sent = 99999;
            screen.status.reports_received = 9999;
            screen.status.heartbeats_sent = 9999;
            screen.status.messages_in = 99999;
            screen.status.messages_out = 99999;
            screen.status.app_strength_a = 10;
            screen.status.app_strength_b = 10;
            screen.status.app_limit_a = 80;
            screen.status.app_limit_b = 80;
            snprintf((char*)screen.status.ip_text, sizeof(screen.status.ip_text),
                "172.20.10.2");
            snprintf((char*)screen.status.controller_id, DGLAB_NET_ID_LEN,
                "8f2a4c1e-9b77-4d21-8c3a-5e6f7a8b9c0d");
            snprintf((char*)screen.status.peer_id, DGLAB_NET_ID_LEN,
                "3c71d0b2-4e55-4a9f-9f1b-2b3c4d5e6f70");
            screen.url = have_qr
                ? "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#"
                  "ws://172.20.10.2:9999/8f2a4c1e-9b77-4d21-8c3a-5e6f7a8b9c0d"
                : "";
            screen.url_ok = have_qr;
            screen.test_strength_a = 100;
            screen.test_strength_b = 100;
            screen.last_command = "waveform A  no app bound";
            screen.last_command_tone = DglabCmdTone_Warn;
            screen.auto_sleep_suppressed = suppressed;
            screen.log_lines = log_lines;
            screen.log_count = DGLAB_SCREEN_LOG_LINES;

            snprintf(name, sizeof(name), "%s socket %u", frames, variant);
            beginPage(&canvas, blue);
            dglabScreenDraw(&canvas, fonts, &screen);
            checkPageStaysInItsRegions(name, blue, PageRegion_Wide);
            checkContentClearsTheBar(name);

            // And the log page it opens with Y: scrolled to the newest line, then
            // scrolled up, which is the only state in which it overflows.
            if (variant == 0) {
                screen.log_open = true;
                screen.log_offset = 0;
                beginPage(&canvas, blue);
                dglabScreenDraw(&canvas, fonts, &screen);
                snprintf(name, sizeof(name), "%s log top", frames);
                checkPageStaysInItsRegions(name, blue, PageRegion_Rows);

                screen.log_offset = 400;
                beginPage(&canvas, blue);
                dglabScreenDraw(&canvas, fonts, &screen);
                snprintf(name, sizeof(name), "%s log scrolled", frames);
                checkPageStaysInItsRegions(name, blue, PageRegion_Rows);
            }
        }

        // The motion page, with and without the right Joy-Con.
        for (unsigned variant = 0; variant < 2; variant++) {
            DglabMotionScreenState motion;
            char name[96];

            memset(&motion, 0, sizeof(motion));
            motion.left_connected = true;
            motion.right_connected = variant == 0;
            motion.moving_a = true;
            motion.level_a = 100;
            motion.frequency_a = 30;
            motion.frequency_b = 1000;
            motion.channel_strength_a = 100;
            motion.channel_strength_b = 0;
            motion.link = "app connected";
            motion.link_tone = DglabCmdTone_Ok;
            motion.last_upload = "waveform A  no app bound";
            motion.last_upload_tone = DglabCmdTone_Error;
            motion.server_running = variant == 0;

            snprintf(name, sizeof(name), "%s motion %u", frames, variant);
            beginPage(&canvas, blue);
            dglabMotionScreenDraw(&canvas, fonts, &motion);
            checkPageStaysInItsRegions(name, blue, PageRegion_Rows);
            checkContentClearsTheBar(name);
        }

        // The touch page, in the states that change what it draws: one half held,
        // both halves held, nothing held, the docked console (which draws no field
        // at all), and a finger in each clamp band beside the density axis - the
        // cases the axis ends exist for.
        for (unsigned variant = 0; variant < 5; variant++) {
            DglabTouchScreenState touch;
            char name[96];

            memset(&touch, 0, sizeof(touch));
            touch.channel_strength_a = 60;
            touch.channel_strength_b = 100;
            touch.link = "app connected";
            touch.link_tone = DglabCmdTone_Ok;
            touch.last_upload = "waveform A  no app bound";
            touch.last_upload_tone = DglabCmdTone_Error;
            touch.server_running = variant != 2;

            if (variant == 0 || variant == 1) {
                touch.held_a = true;
                touch.x_a = 300;
                touch.y_a = 200;
                touch.level_a = 80;
                touch.frequency_a = 40;
            }

            if (variant == 1) {
                touch.held_b = true;
                touch.x_b = 900;
                touch.y_b = 500;
                touch.level_b = 26;
                touch.frequency_b = 70;
            }

            if (variant == 4) {
                // Past both ends of the density axis, and at both ends of the
                // value axis: the marker is clamped into the field, and the
                // numbers are the extremes (far left sparsest, far right densest).
                touch.held_a = true;
                touch.x_a = 10;
                touch.y_a = 0;
                touch.level_a = 100;
                touch.frequency_a = 100;
                touch.held_b = true;
                touch.x_b = DGLAB_TOUCH_PANEL_WIDTH - 10;
                touch.y_b = DGLAB_TOUCH_PANEL_HEIGHT - 1;
                touch.level_b = 0;
                touch.frequency_b = 30;
            }

            touch.docked = variant == 3;

            snprintf(name, sizeof(name), "%s touch %u", frames, variant);
            beginPage(&canvas, blue);
            dglabTouchScreenDraw(&canvas, fonts, &touch);
            checkPageStaysInItsRegions(name, blue, PageRegion_Playfield);

            // The field runs to the rules on purpose - it is the input - so the
            // "content clears the bar" check cannot read that band the way it
            // reads a page of rows: the field's own fixed lines are masked out
            // instead, and anything else down there is a row that did not fit.
            checkFieldClearsTheBar(name);

            if (variant == 3) {
                checkDockedField(name);
            } else {
                checkFieldStaysInsideTheAxis(name);
            }
        }

        // The advanced page, on the first and the last setting: the description is
        // what makes one page taller than the other.
        {
            static const unsigned settings[] = { DglabMotionSetting_DeadzoneEnter,
                DglabMotionSetting_StrengthMax };
            DglabAdvancedState advanced;

            for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
                char name[96];

                memset(&advanced, 0, sizeof(advanced));
                advanced.config = &config;
                advanced.selected = settings[i];
                advanced.saved = i == 0;

                snprintf(name, sizeof(name), "%s advanced %u", frames, settings[i]);
                beginPage(&canvas, blue);
                dglabAdvancedDraw(&canvas, fonts, &advanced);
                checkPageStaysInItsRegions(name, blue, PageRegion_Rows);
            }
        }

        // The about page, with the longest url the row can carry: at the top, and
        // scrolled well past the end of its content - which the page clamps - so
        // the bar and the last rows are drawn in both states. This page scrolls,
        // so it is allowed to run up to the clip edge like the menu does.
        for (unsigned variant = 0; variant < 2; variant++) {
            DglabAboutState about;
            char name[96];

            memset(&about, 0, sizeof(about));
            about.preference = DglabLanguage_Auto;
            about.resolved = DglabLanguage_ChineseSimplified;
            about.app_version = "0.3.0";
            // The longest stamp `git describe --always --dirty` produces, so the
            // header and the row are measured against the real thing.
            about.build_id = "8a2fcb6-dirty";
            about.ipc_version.major = 1;
            about.ipc_version.minor = 2;
            about.ipc_version.patch = 3;
            about.github_url = "https://github.com/livcm/DGLAB-NX";
            about.sysmodule_ok = (variant % 2) == 0;
            about.offset = variant == 0 ? 0 : 400;

            snprintf(name, sizeof(name), "%s about %u", frames, variant);
            beginPage(&canvas, blue);
            dglabAboutDraw(&canvas, fonts, &about);
            checkPageStaysInItsRegions(name, blue, PageRegion_Rows);
        }
    }
}

// The three things the About page exists to tell apart: the release version of
// this NRO (its own row, since the header carries the sysmodule state like every
// other page), the IPC interface version of the sysmodule, and the build stamp.
// They arrive in three separate fields, and the page has to draw all three: drop
// any one of them and the frame changes.
//
// This source has one glyph for every character, so what the page drew cannot be
// read back - but a longer value is drawn wider, and a missing one is not drawn
// at all, and that is what is compared here. A field the page ignores is a field
// that only ever shows "unknown" on a console, which is the failure this catches.
static void testAboutShowsItsThreeVersions(void)
{
    static uint8_t base[DOCK_PIXEL_WIDTH * DOCK_PIXEL_HEIGHT * 4];
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabFontSet fonts = blockFonts();
    DglabAboutState about;
    DglabCanvas canvas;

    setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);

    memset(&about, 0, sizeof(about));
    about.preference = DglabLanguage_English;
    about.resolved = DglabLanguage_English;
    about.app_version = "1.2.3";
    about.build_id = "aaaaaaa";
    about.ipc_version.major = 4;
    about.ipc_version.minor = 5;
    about.ipc_version.patch = 6;
    about.github_url = "https://github.com/livcm/DGLAB-NX";

    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    memcpy(base, g_screen_pixels, screenBytes());

    // The release version is a row of its own, drawn only when there is one: a
    // build that never got one shows exactly this.
    about.app_version = "";
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    CHECK(countDifferingPixels(base, g_screen_pixels, screenBytes()) > 0);
    about.app_version = "1.2.3";

    about.build_id = "";
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    CHECK(countDifferingPixels(base, g_screen_pixels, screenBytes()) > 0);
    about.build_id = "aaaaaaa";

    // The values are right aligned, so a longer one starts further left.
    about.ipc_version.major = 44;
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    CHECK(countDifferingPixels(base, g_screen_pixels, screenBytes()) > 0);
    about.ipc_version.major = 4;

    // In this font the page is taller than the screen in both languages (one of
    // the two paragraphs wraps at this column width), which is what the scroll
    // keys and the bottom bar's scroll hint are for. On a console the real system
    // font is narrower and the page fits, so this is the branch the pages
    // themselves cannot be relied on to reach; a wording change that made the
    // page fit here as well would leave them as dead weight, so it fails here.
    CHECK(dglabAboutContentHeight(&fonts, &about) >
          DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP);

    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    memcpy(base, g_screen_pixels, screenBytes());

    // Scrolling moves the rows, and the console's scrollbar says how far there is
    // left to go: it is drawn in its own column on the right edge.
    about.offset = 400;
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    CHECK(countDifferingPixels(base, g_screen_pixels, screenBytes()) > 0);

    {
        int bar_x = DGLAB_PAGE_WIDTH - 17;
        int bar = 0;

        for (int y = DGLAB_PAGE_CONTENT_TOP; y < DGLAB_PAGE_CONTENT_BOTTOM; y++) {
            for (int x = bar_x; x < bar_x + 4; x++) {
                if (screenPixel(x, y) == dglabThemeGet()->scrollbar)
                    bar++;
            }
        }

        CHECK(bar > 0);
    }
}

// The colour theme is a page input of the About screen - Y cycles it and the row
// says which value it took - so the row has to be drawn, and the palette the
// preference resolves to has to be the one the page is drawn with. A field that
// reaches the state struct but not the screen is exactly what these renders
// catch.
//
// The English strings are used on purpose: the host's block font draws one solid
// rectangle per character, so two values of the same length are the same pixels
// (浅色 and 深色 are), and only the English pair has different lengths.
static void testAboutShowsItsThemeRow(void)
{
    static uint8_t base[DOCK_PIXEL_WIDTH * DOCK_PIXEL_HEIGHT * 4];
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabFontSet fonts = blockFonts();
    DglabAboutState about;
    DglabCanvas canvas;

    dglabStringsSetLanguage(DglabLanguage_English);
    setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);

    memset(&about, 0, sizeof(about));
    about.preference = DglabLanguage_English;
    about.resolved = DglabLanguage_English;
    about.theme = DglabThemeMode_Auto;
    about.theme_system_is_dark = true;
    about.app_version = "0.3.0";
    about.build_id = "8a2fcb6";
    about.ipc_version.major = 1;
    about.ipc_version.minor = 2;
    about.ipc_version.patch = 3;
    about.github_url = "https://github.com/livcm/DGLAB-NX";
    // The theme row is the last one, and in the host font the page is taller than
    // the screen: scrolled to the end (the page clamps the offset) is the only
    // state in which the row is on screen at all.
    about.offset = 1000;

    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    memcpy(base, g_screen_pixels, screenBytes());

    // Auto draws "Follow the system (Dark)" here, which is wider than "Light":
    // the row follows the preference.
    about.theme = DglabThemeMode_Light;
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    CHECK(countDifferingPixels(base, g_screen_pixels, screenBytes()) > 0);

    // And the fixed values are their own text, not one "not auto" state.
    memcpy(base, g_screen_pixels, screenBytes());
    about.theme = DglabThemeMode_Dark;
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    CHECK(countDifferingPixels(base, g_screen_pixels, screenBytes()) > 0);

    // The same rows on the light table: the preference picks the palette the
    // whole page is painted with, not just the words in the row.
    about.theme = DglabThemeMode_Auto;
    dglabThemeSet(NULL);
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    memcpy(base, g_screen_pixels, screenBytes());

    dglabThemeSet(&dglabThemeLight);
    beginPage(&canvas, blue);
    dglabAboutDraw(&canvas, &fonts, &about);
    CHECK(countDifferingPixels(base, g_screen_pixels, screenBytes()) > 0);
    dglabThemeSet(NULL);
}

// "A bar only when the content is taller than the page" is one rule, and it
// lives in one function - so the branch a page cannot reach on its own is
// checked here directly. (In the host font both language files make the about
// page taller than the screen, so its own renders never reach "it fits".)
static void testListPageLayout(void)
{
    DglabListPage page;

    // Shorter than the view: nothing to scroll, and a caller that asks anyway is
    // clamped back to the top rather than scrolling into empty space.
    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, 519, 400, 0);
    CHECK(page.max_offset == 0);
    CHECK(!dglabListPageScrolls(&page));
    CHECK(page.offset == 0);

    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, 519, 400, 90);
    CHECK(page.offset == 0);

    // Taller than the view: the bar is drawn and the offset stops at the bottom.
    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, 519, 553, 0);
    CHECK(page.max_offset == 34);
    CHECK(dglabListPageScrolls(&page));

    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, 519, 553, 400);
    CHECK(page.offset == 34);

    page = dglabListPageLayout(DGLAB_PAGE_CONTENT_TOP, 519, 553, -20);
    CHECK(page.offset == 0);

    // The same clamp, on its own: it is what main.c uses to keep a page's own
    // scroll offset inside the content.
    CHECK(dglabListScrollClamp(-5, 100) == 0);
    CHECK(dglabListScrollClamp(40, 100) == 40);
    CHECK(dglabListScrollClamp(400, 100) == 100);
    CHECK(dglabListScrollClamp(5, -20) == 0);
}

// One page of the status check below: the state each screen needs, built fresh,
// so the only thing that differs between two renders of the same page is
// `sysmodule_ok`. Page 2 is the log sub-page, i.e. the socket page with its log
// open.
static void drawStatusProbe(unsigned page, bool ok, const DglabFontSet* fonts)
{
    static const char* const log_lines[2] = { "socket server core ready", "app bound" };
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabMotionFeedConfig config;
    DglabAboutState about;
    DglabAdvancedState advanced;
    DglabMenuState menu;
    DglabMotionScreenState motion;
    DglabTouchScreenState touch;
    DglabScreenState screen;
    DglabCanvas canvas;

    dglabMotionSettingsDefault(&config);

    memset(&menu, 0, sizeof(menu));
    memset(&screen, 0, sizeof(screen));
    memset(&motion, 0, sizeof(motion));
    memset(&advanced, 0, sizeof(advanced));
    memset(&about, 0, sizeof(about));
    memset(&touch, 0, sizeof(touch));

    menu.sysmodule_ok = ok;

    screen.sysmodule_ok = ok;
    screen.status_ok = true;
    screen.status.state = DglabNetState_Paired;
    screen.log_lines = log_lines;
    screen.log_count = 2;

    motion.sysmodule_ok = ok;
    motion.link = dglabNetStateText(DglabNetState_Paired);
    motion.last_upload = "-";

    advanced.sysmodule_ok = ok;
    advanced.config = &config;
    advanced.saved = true;

    about.sysmodule_ok = ok;
    about.app_version = "0.3.0";
    about.build_id = "8a2fcb6";
    about.github_url = "https://github.com/livcm/DGLAB-NX";

    touch.sysmodule_ok = ok;
    touch.link = dglabNetStateText(DglabNetState_Paired);
    touch.last_upload = "-";
    touch.held_a = true;
    touch.x_a = 300;
    touch.y_a = 200;
    touch.level_a = 80;
    touch.frequency_a = 40;

    beginPage(&canvas, blue);

    switch (page) {
        case 0: dglabMenuDraw(&canvas, fonts, &menu); break;
        case 1: dglabScreenDraw(&canvas, fonts, &screen); break;
        case 2:
            screen.log_open = true;
            dglabScreenDraw(&canvas, fonts, &screen);
            break;
        case 3: dglabMotionScreenDraw(&canvas, fonts, &motion); break;
        case 4: dglabTouchScreenDraw(&canvas, fonts, &touch); break;
        case 5: dglabAdvancedDraw(&canvas, fonts, &advanced); break;
        default: dglabAboutDraw(&canvas, fonts, &about); break;
    }
}

// Every page's title bar carries the sysmodule state, and it is the same line
// everywhere because one function draws it. Flipping the flag has to change the
// frame, and every pixel that changes has to be in the header band: a page that
// put its status anywhere else, or that shows a status of its own, fails here.
static void testEveryPageShowsTheSysmoduleStatus(void)
{
    static const char* const names[] = { "menu", "socket", "log", "motion", "touch", "advanced",
        "about" };
    static uint8_t base[DOCK_PIXEL_WIDTH * DOCK_PIXEL_HEIGHT * 4];
    DglabFontSet fonts = blockFonts();

    setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);

    // Both palettes: the status line is drawn in the theme's accent (or its
    // error colour) either way, so the rule holds on the light table too.
    for (unsigned mode = 0; mode < 2; mode++) {
        dglabThemeSet(mode == 0 ? NULL : &dglabThemeLight);

        for (unsigned page = 0; page < sizeof(names) / sizeof(names[0]); page++) {
            int header_bottom = dglabCanvasScale(&g_page_canvas, DGLAB_PAGE_RULE_Y);
            int changed = 0;
            int outside = 0;

            drawStatusProbe(page, true, &fonts);
            memcpy(base, g_screen_pixels, screenBytes());

            drawStatusProbe(page, false, &fonts);

            for (int y = 0; y < g_screen_height; y++) {
                for (int x = 0; x < g_screen_width; x++) {
                    size_t offset = ((size_t)y * (size_t)g_screen_width + (size_t)x) * 4u;

                    if (memcmp(base + offset, g_screen_pixels + offset, 4) == 0)
                        continue;

                    changed++;

                    if (y > header_bottom) {
                        if (outside < 4)
                            printf("    %s %s: %d,%d changed outside the header\n",
                                mode == 0 ? "dark" : "light", names[page], x, y);

                        outside++;
                    }
                }
            }

            if (changed == 0 || outside)
                printf("  %s %s: %d pixels changed, %d of them below the header\n",
                    mode == 0 ? "dark" : "light", names[page], changed, outside);

            CHECK(changed > 0);
            CHECK(outside == 0);
        }
    }

    dglabThemeSet(NULL);
}

// The same suite for every language, for both palettes, and for both frames the
// NRO draws into: the handheld 720p one, and the docked 1080p one whose whole
// point is that the layout is unchanged. The light theme is not a second layout
// - it is the same one with the other table - so it has to pass exactly the same
// check, rules and background included.
static void testEveryPageStaysInItsRegions(void)
{
    static const DglabLanguage languages[2] = { DglabLanguage_English,
        DglabLanguage_ChineseSimplified };
    static const DglabThemeMode themes[2] = { DglabThemeMode_Dark, DglabThemeMode_Light };
    DglabFontSet handheld = blockFonts();
    DglabFontSet docked = dockFonts();

    for (size_t lang = 0; lang < sizeof(languages) / sizeof(languages[0]); lang++) {
        dglabStringsSetLanguage(languages[lang]);

        for (size_t mode = 0; mode < sizeof(themes) / sizeof(themes[0]); mode++) {
            char frames[32];

            dglabThemeSet(dglabThemeResolve(themes[mode], true));

            setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);
            snprintf(frames, sizeof(frames), "720p %s",
                themes[mode] == DglabThemeMode_Light ? "light" : "dark");
            checkEveryPage(frames, &handheld);

            setScreenSize(DOCK_PIXEL_WIDTH, DOCK_PIXEL_HEIGHT, DOCK_SCALE_NUM, DOCK_SCALE_DEN);
            snprintf(frames, sizeof(frames), "1080p %s",
                themes[mode] == DglabThemeMode_Light ? "light" : "dark");
            checkEveryPage(frames, &docked);
        }
    }

    // The other tests and the previews assume the default table.
    dglabThemeSet(NULL);
    setScreenSize(SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, 1, 1);
    dglabStringsSetLanguage(DglabLanguage_English);
}

int main(void)
{
    // The screens draw the text of the language files, so they are loaded the
    // way the NRO loads them: from lang/*.json rather than from the binary.
    char message[512] = { 0 };

    if (!dglabTestLoadLanguages(message, sizeof(message))) {
        printf("FAIL %s\n", message);
        return 1;
    }

    testFillAndClip();
    testFrame();
    testText();
    testScaledCanvas();
    testWrap();
    testBlend();
    testButtonIcons();
    testButtonIconLetterMargins();
    testHintIconUsesTheIconFont();
    testFirstRowFocusRingIsWhole();
    testQr();
    testQrOnlyWhileTheServerRuns();
    testScreen();
    testMenu();
    testMotionScreen();
    testAdvancedScreen();
    testAboutShowsItsThreeVersions();
    testAboutShowsItsThemeRow();
    testListPageLayout();
    testEveryPageShowsTheSysmoduleStatus();
    testEveryPageStaysInItsRegions();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
