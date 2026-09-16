// Host side tests for the NRO canvas.
//
// The canvas is what the framebuffer backend and (later) a deko3d backend draw
// through, so the clipping and the bitmap font layout are checked here rather
// than on the console.
//
// Run with: make -C tests/canvas

#include <dglab/ui/canvas.h>
#include <dglab/ui/advanced.h>
#include <dglab/ui/menu.h>
#include <dglab/ui/motion.h>
#include <dglab/ui/screen.h>
#include <dglab/nro/motion_settings.h>

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

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

    // drawLine in screen.c puts the value column 12 characters in and the server
    // panel leaves room for 21, so the two strengths together are the widest
    // string that has to fit.
    CHECK(dglabCanvasTextWidth(&kFont, 1, "A 100/100  B 100/100") <= 21 * 16);
    // Same budget for the command feedback line: "A test  ok (A is 0)" is the
    // longest form main.c can build.
    CHECK(dglabCanvasTextWidth(&kFont, 1, "A test  ok (A is 0)") <= 21 * 16);

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

    dglabScreenDraw(&canvas, &kFont, &state);

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
    DglabGlyphSource* source = dglabBitmapGlyphSource(&kFont);
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
        dglabMenuDraw(&canvas, source, &menu);

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

    // The row is "A" + two spaces + the value, and the value is the widest form
    // the drawing code can produce.
    CHECK(dglabCanvasTextWidth(&kFont, 1, "moving     level 100    30ms") <= 34 * 16);

    dglabCanvasInit(&canvas, screen_pixels, 1280, 720, 1280 * 4);
    dglabCanvasFill(&canvas, 0, 0, 1280, 720, blue);
    dglabMotionScreenDraw(&canvas, &kFont, &state);

    changed = countChangedPixels(screen_pixels, sizeof(screen_pixels), blue);
    CHECK(changed > 1280 * 720 / 2);
}

// The advanced screen: the panel is sized for the longest description, so that
// assumption is checked here rather than discovered as text spilling over the
// border on a console.
static void testAdvancedScreen(void)
{
    static uint8_t screen_pixels[1280 * 720 * 4];
    const uint32_t blue = DGLAB_RGBA(0, 0, 0xFF, 0xFF);
    DglabMotionFeedConfig config;
    DglabAdvancedState state;
    DglabCanvas canvas;
    int changed;

    // 5 lines of 40 characters is what the panel leaves for the description;
    // the drawing code wraps at 42 columns and the rest is margin.
    for (unsigned setting = 0; setting < (unsigned)DglabMotionSetting_Count; setting++) {
        CHECK(dglabMotionSettingName(setting)[0] != '\0');
        CHECK(strlen(dglabMotionSettingDescription(setting)) <= 5 * 40);
    }

    dglabMotionSettingsDefault(&config);
    dglabMotionSettingsStep(&config, DglabMotionSetting_FrequencyFast, -4);

    memset(&state, 0, sizeof(state));
    state.config = &config;
    state.selected = DglabMotionSetting_FrequencyFast;
    state.saved = true;

    dglabCanvasInit(&canvas, screen_pixels, 1280, 720, 1280 * 4);
    dglabCanvasFill(&canvas, 0, 0, 1280, 720, blue);
    dglabAdvancedDraw(&canvas, &kFont, &state);

    changed = countChangedPixels(screen_pixels, sizeof(screen_pixels), blue);
    CHECK(changed > 1280 * 720 / 2);
}

int main(void)
{
    testFillAndClip();
    testFrame();
    testText();
    testQr();
    testScreen();
    testMenu();
    testMotionScreen();
    testAdvancedScreen();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
