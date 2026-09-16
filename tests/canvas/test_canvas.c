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

static DglabFontSet blockFonts(void);

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
    state.version.minor = 2;
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

// The mode menu: every entry draws, the selection wraps both ways, and a full
// circle of moves comes back to where it started.
static void testMenu(void)
{
    static uint8_t screen_pixels[1280 * 720 * 4];
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabFontSet fonts = blockFonts();
    DglabMenuState menu;
    DglabCanvas canvas;

    CHECK(dglabMenuMove(0, -1) == DglabMenu_ItemCount - 1);
    CHECK(dglabMenuMove(DglabMenu_ItemCount - 1, 1) == 0);
    CHECK(dglabMenuMove(DglabMenu_ItemCount - 1, -1) == DglabMenu_ItemCount - 2);

    for (unsigned item = 0; item < (unsigned)DglabMenu_ItemCount; item++) {
        int changed;

        CHECK(dglabMenuItemName(item)[0] != '\0');
        CHECK(dglabMenuItemDescription(item)[0] != '\0');
        CHECK(dglabMenuMove(item, (int)DglabMenu_ItemCount) == item);

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
    static uint8_t screen_pixels[1280 * 720 * 4];
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
    // the longest of each has to fit the content column together. The label is
    // the localised string, so this covers the Chinese table too.
    {
        int labels = dglabCanvasTextWidth(&kFont, 1, dglabString(DglabString_MotionVolume));
        int inside = DGLAB_PAGE_CONTENT_WIDTH;
        int widest = dglabCanvasTextWidth(&kFont, 1, "moving   waveform 100   100ms");

        CHECK(labels + 24 + widest <= inside);
    }

    dglabCanvasInit(&canvas, screen_pixels, 1280, 720, 1280 * 4);
    dglabCanvasFill(&canvas, 0, 0, 1280, 720, blue);
    {
        DglabFontSet fonts = blockFonts();

        dglabMotionScreenDraw(&canvas, &fonts, &state);
    }

    changed = countChangedPixels(screen_pixels, sizeof(screen_pixels), blue);
    CHECK(changed > 1280 * 720 / 2);
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

#define SCREEN_PIXEL_WIDTH 1280
#define SCREEN_PIXEL_HEIGHT 720

static uint8_t g_screen_pixels[SCREEN_PIXEL_WIDTH * SCREEN_PIXEL_HEIGHT * 4];

// The four sizes a screen draws with. The host has no system font, so every size
// is the block font: what these tests check is where the layout puts things, and
// that does not depend on the face.
static DglabFontSet blockFonts(void)
{
    DglabFontSet fonts;

    fonts.title = blockSource();
    fonts.body = fonts.title;
    fonts.value = fonts.title;
    fonts.note = fonts.title;

    return fonts;
}

static uint32_t screenPixel(int x, int y)
{
    size_t offset = ((size_t)y * SCREEN_PIXEL_WIDTH + (size_t)x) * 4u;

    return ((uint32_t)g_screen_pixels[offset] << 24) |
           ((uint32_t)g_screen_pixels[offset + 1] << 16) |
           ((uint32_t)g_screen_pixels[offset + 2] << 8) | (uint32_t)g_screen_pixels[offset + 3];
}

static void beginPage(DglabCanvas* canvas, uint32_t background)
{
    dglabCanvasInit(canvas, g_screen_pixels, SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT,
        SCREEN_PIXEL_WIDTH * 4);
    dglabCanvasFill(canvas, 0, 0, SCREEN_PIXEL_WIDTH, SCREEN_PIXEL_HEIGHT, background);
}

// Where a page may draw: the header band, the content column, the bottom bar, and
// the scrollbar's own column on the right. A pixel outside all four is a layout
// that grew past what it was measured for.
//
// This is the console page's version of "nothing on the frame": the old screens
// were checked for text against a panel's border, and there is no border any
// more, so what has to hold is that nothing leaves the column it belongs to.
// The one pixel of slack absorbs the antialiased edge of the focus ring.
static bool pageRegion(int x, int y, bool wide)
{
    if (y <= DGLAB_PAGE_RULE_Y)
        return true;

    if (y >= DGLAB_PAGE_BAR_Y)
        return true;

    if (x >= DGLAB_PAGE_WIDTH - 20)
        return true;

    // A one column page keeps to x=220..1060; a two column page uses the wider
    // band, because that is what the console does (docs/nro-ui.md). The clip
    // starts one pixel under the title rule, so a focused first row's ring is
    // drawn whole.
    if (wide && x >= DGLAB_PAGE_WIDE_X - 1 &&
        x <= DGLAB_PAGE_WIDE_X + DGLAB_PAGE_WIDE_WIDTH)
        return y >= DGLAB_PAGE_CLIP_TOP - 1 && y < DGLAB_PAGE_CONTENT_BOTTOM;

    return x >= DGLAB_PAGE_CONTENT_X - 1 &&
           x <= DGLAB_PAGE_CONTENT_X + DGLAB_PAGE_CONTENT_WIDTH &&
           y >= DGLAB_PAGE_CLIP_TOP - 1 && y < DGLAB_PAGE_CONTENT_BOTTOM;
}

static void checkPageStaysInItsRegions(const char* name, uint32_t background, bool wide)
{
    // The page fills itself with the theme's own background, so both colours are
    // "nothing was drawn here": what the check looks for is ink.
    uint32_t page_background = dglabThemeGet()->background;
    int outside = 0;
    int drawn = 0;

    for (int y = 0; y < SCREEN_PIXEL_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_PIXEL_WIDTH; x++) {
            uint32_t pixel = screenPixel(x, y);

            if (pixel == background || pixel == page_background)
                continue;

            drawn++;

            if (pageRegion(x, y, wide))
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

    // Both rules are white and one pixel tall, and the margin beside them is
    // left alone: the console reserves the grey #4D4D4D for the rows inside the
    // page (docs/nro-ui.md).
    CHECK(screenPixel(640, DGLAB_PAGE_RULE_Y) == dglabThemeGet()->rule);
    CHECK(screenPixel(640, DGLAB_PAGE_BAR_Y) == dglabThemeGet()->rule);
    CHECK(screenPixel(24, DGLAB_PAGE_BAR_Y - 2) == page_background);
    CHECK(screenPixel(24, DGLAB_PAGE_BAR_Y + 2) == page_background);
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

    CHECK(pixelAt(13, 0) == theme.background);  // above the disc
    CHECK(pixelAt(1, 13) == icon);              // the disc's own fill
    CHECK(pixelAt(13, 13) == theme.background); // the knocked out letter
    CHECK(pixelAt(0, 0) == theme.background);   // the corner outside the circle

    // The two shoulders are wider than a face button, so a ZL+ZR hint reads as
    // two icons rather than as one wide one.
    CHECK(dglabButtonIconWidth(DglabButton_ZL) > dglabButtonIconWidth(DglabButton_A));

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

    for (int y = DGLAB_PAGE_BAR_Y - 6; y < DGLAB_PAGE_BAR_Y; y++) {
        for (int x = DGLAB_PAGE_CONTENT_X; x < DGLAB_PAGE_CONTENT_X + DGLAB_PAGE_CONTENT_WIDTH;
             x++) {
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

// Every page, in both languages, with the longest state each one can be given:
// the socket page with a bound app and a full log, the advanced page on its
// last setting, and the menu on every entry in turn. What is checked is where
// the ink ended up, not what it says.
static void testEveryPageStaysInItsRegions(void)
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
    static const DglabLanguage languages[2] = { DglabLanguage_English,
        DglabLanguage_ChineseSimplified };
    DglabFontSet fonts = blockFonts();
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabMotionFeedConfig config;
    DglabCanvas canvas;

    dglabMotionSettingsDefault(&config);

    for (size_t lang = 0; lang < sizeof(languages) / sizeof(languages[0]); lang++) {
        char name[64];

        dglabStringsSetLanguage(languages[lang]);

        // The menu, on every entry: the description under the selected one is
        // what changes the page's height.
        for (unsigned item = 0; item < (unsigned)DglabMenu_ItemCount; item++) {
            DglabMenuState menu;

            memset(&menu, 0, sizeof(menu));
            menu.selected = item;
            menu.sysmodule_ok = (item % 2) == 0;

            snprintf(name, sizeof(name), "menu %u", item);
            beginPage(&canvas, blue);
            dglabMenuDraw(&canvas, &fonts, &menu);
            checkPageStaysInItsRegions(name, blue, false);
        }

        // The socket page, in the four states that change what it draws: the
        // server running or not, and a QR code available or not. The log page is
        // part of the same view.
        for (unsigned variant = 0; variant < 4; variant++) {
            bool running = (variant & 1) != 0;
            bool have_qr = (variant & 2) != 0;
            DglabScreenState screen;

            memset(&screen, 0, sizeof(screen));
            screen.status_ok = true;
            screen.version.minor = 2;
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
            screen.log_lines = log_lines;
            screen.log_count = DGLAB_SCREEN_LOG_LINES;

            snprintf(name, sizeof(name), "socket %u", variant);
            beginPage(&canvas, blue);
            dglabScreenDraw(&canvas, &fonts, &screen);
            checkPageStaysInItsRegions(name, blue, true);
            checkContentClearsTheBar(name);

            // And the log page it opens with Y: scrolled to the newest line, then
            // scrolled up, which is the only state in which it overflows.
            if (variant == 0) {
                screen.log_open = true;
                screen.log_offset = 0;
                beginPage(&canvas, blue);
                dglabScreenDraw(&canvas, &fonts, &screen);
                checkPageStaysInItsRegions("log top", blue, false);

                screen.log_offset = 400;
                beginPage(&canvas, blue);
                dglabScreenDraw(&canvas, &fonts, &screen);
                checkPageStaysInItsRegions("log scrolled", blue, false);
            }
        }

        // The motion page, with and without the right Joy-Con.
        for (unsigned variant = 0; variant < 2; variant++) {
            DglabMotionScreenState motion;

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

            snprintf(name, sizeof(name), "motion %u", variant);
            beginPage(&canvas, blue);
            dglabMotionScreenDraw(&canvas, &fonts, &motion);
            checkPageStaysInItsRegions(name, blue, false);
            checkContentClearsTheBar(name);
        }

        // The advanced page, on the first and the last setting: the description is
        // what makes one page taller than the other.
        {
            static const unsigned settings[] = { DglabMotionSetting_DeadzoneEnter,
                DglabMotionSetting_StrengthMax };
            DglabAdvancedState advanced;

            for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
                memset(&advanced, 0, sizeof(advanced));
                advanced.config = &config;
                advanced.selected = settings[i];
                advanced.saved = i == 0;

                snprintf(name, sizeof(name), "advanced %u", settings[i]);
                beginPage(&canvas, blue);
                dglabAdvancedDraw(&canvas, &fonts, &advanced);
                checkPageStaysInItsRegions(name, blue, false);
            }
        }

        // The about page, with the longest url the row can carry.
        {
            DglabAboutState about;

            memset(&about, 0, sizeof(about));
            about.preference = DglabLanguage_Auto;
            about.resolved = languages[lang];
            about.version.major = 1;
            about.version.minor = 2;
            about.version.patch = 3;
            about.github_url = "https://github.com/livcm/DGLAB-NX";

            beginPage(&canvas, blue);
            dglabAboutDraw(&canvas, &fonts, &about);
            checkPageStaysInItsRegions("about", blue, false);
            checkContentClearsTheBar("about");
        }
    }

    // The other tests and the previews assume the default table.
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
    testWrap();
    testBlend();
    testButtonIcons();
    testFirstRowFocusRingIsWhole();
    testQr();
    testScreen();
    testMenu();
    testMotionScreen();
    testAdvancedScreen();
    testEveryPageStaysInItsRegions();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
