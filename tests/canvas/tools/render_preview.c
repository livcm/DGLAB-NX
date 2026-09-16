// Renders the NRO screen on a PC so the layout can be checked without a
// console. It is a development tool, not part of the build.
//
// The font is libnx's default console font. Extract it once with:
//
//   AR=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar
//   $AR p $DEVKITPRO/libnx/lib/libnx.a default_font.bin.o > default_font.bin.o
//   $DEVKITPRO/devkitA64/bin/aarch64-none-elf-objcopy -O binary \
//       --only-section=.rodata.default_font_bin default_font.bin.o font.bin
//
// Use devkitA64's ar rather than the host one: the host ar (llvm ar on macOS)
// does not resolve this archive's member names and reports "not found".
//
// Then:
//
//   cc -std=c11 -I nro/include -I common/include -I tests/net/hostshim \
//       tests/canvas/tools/render_preview.c nro/source/ui/*.c nro/source/motion/*.c \
//       -o /tmp/preview -lm
//   /tmp/preview /tmp/font.bin /tmp/preview.bmp
//   /tmp/preview /tmp/font.bin /tmp/nowifi.bmp nowifi   (no LAN address yet)
//   /tmp/preview /tmp/font.bin /tmp/stopped.bmp stopped (server not started)
//   /tmp/preview /tmp/font.bin /tmp/menu.bmp menu       (the mode menu)
//   /tmp/preview /tmp/font.bin /tmp/motion.bmp motion   (the Joy-Con mode)
//   /tmp/preview /tmp/font.bin /tmp/advanced.bmp advanced  (the motion parameters)
//   sips -s format png /tmp/preview.bmp --out /tmp/preview.png

#include <dglab/ui/screen.h>
#include <dglab/ui/advanced.h>
#include <dglab/ui/menu.h>
#include <dglab/ui/motion.h>
#include <dglab/nro/motion_settings.h>
#include <dglab/ui/about.h>
#include <dglab/ui/text_ttf.h>
#include <dglab/ui/language.h>
#include <dglab/ui/strings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 1280
#define HEIGHT 720

static uint8_t g_font_data[8192];
static uint8_t g_pixels[WIDTH * HEIGHT * 4];
static DglabGlyphSource* g_text;

static void writeBmp(const char* path)
{
    int row_size = (WIDTH * 3 + 3) & ~3;
    int data_size = row_size * HEIGHT;
    int file_size = 54 + data_size;
    unsigned char header[54];
    FILE* out = fopen(path, "wb");
    unsigned char* row = malloc((size_t)row_size);

    if (out == NULL || row == NULL)
        return;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    memcpy(header + 2, &file_size, 4);
    {
        int offset = 54;
        int dib = 40;
        short planes = 1;
        short bpp = 24;

        memcpy(header + 10, &offset, 4);
        memcpy(header + 14, &dib, 4);
        memcpy(header + 18, &(int){ WIDTH }, 4);
        memcpy(header + 22, &(int){ HEIGHT }, 4);
        memcpy(header + 26, &planes, 2);
        memcpy(header + 28, &bpp, 2);
        memcpy(header + 34, &data_size, 4);
    }

    fwrite(header, 1, sizeof(header), out);

    // BMP rows run bottom to top.
    for (int y = HEIGHT - 1; y >= 0; y--) {
        memset(row, 0, (size_t)row_size);

        for (int x = 0; x < WIDTH; x++) {
            const uint8_t* pixel = g_pixels + ((size_t)y * WIDTH + x) * 4;

            row[x * 3 + 0] = pixel[2];
            row[x * 3 + 1] = pixel[1];
            row[x * 3 + 2] = pixel[0];
        }

        fwrite(row, 1, (size_t)row_size, out);
    }

    free(row);
    fclose(out);
}

int main(int argc, char** argv)
{
    static const char* log_lines[] = {
        "socket server core ready, controller id 8f2a4c1e-...",
        "listening on port 9999",
        "app 3c71d0b2-... bound",
        "rx msg: strength-10+10+80+80",
        "tx command 1 channel 0 value 15",
        "app 3c71d0b2-... has been quiet for 90 s",
    };
    DglabFont font;
    DglabScreenState state;
    DglabCanvas canvas;
    char url[DGLAB_NET_QR_MAX];
    FILE* file;

    if (argc < 3) {
        fprintf(stderr, "usage: render_preview <font.bin> <out.bmp> "
                        "[normal|nowifi|stopped|menu|motion|advanced]\n");
        return 2;
    }

    file = fopen(argv[1], "rb");

    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 3;
    }

    if (fread(g_font_data, 1, sizeof(g_font_data), file) != sizeof(g_font_data)) {
        fprintf(stderr, "%s is not the 8192 byte libnx font\n", argv[1]);
        fclose(file);
        return 4;
    }

    fclose(file);

    font.glyphs = g_font_data;
    font.ascii_offset = 0;
    font.glyph_count = 256;
    font.tile_width = 16;
    font.tile_height = 16;

    memset(&state, 0, sizeof(state));
    state.status_ok = true;
    state.version.major = 0;
    state.version.minor = 2;
    state.status.state = DglabNetState_Paired;
    state.status.port = 9999;
    state.status.sessions = 3;
    state.status.commands_sent = 7;
    state.status.reports_received = 4;
    state.status.heartbeats_sent = 2;
    state.status.messages_in = 9;
    state.status.messages_out = 12;
    state.status.app_strength_a = 10;
    state.status.app_strength_b = 10;
    state.status.app_limit_a = 80;
    state.status.app_limit_b = 80;
    state.status.app_feedback = DGLAB_NET_FEEDBACK_NONE;
    state.test_strength_a = 20;
    state.test_strength_b = 0;
    state.last_command = "A test  ok (A is 0)";
    state.last_command_tone = DglabCmdTone_Warn;
    state.log_lines = log_lines;
    state.log_count = 6;

    snprintf((char*)state.status.ip_text, sizeof(state.status.ip_text), "192.168.1.161");
    snprintf((char*)state.status.controller_id, DGLAB_NET_ID_LEN,
        "8f2a4c1e-9b77-4d21-8c3a-5e6f7a8b9c0d");
    snprintf((char*)state.status.peer_id, DGLAB_NET_ID_LEN,
        "3c71d0b2-4e55-4a9f-9f1b-2b3c4d5e6f70");

    snprintf(url, sizeof(url),
        "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#"
        "ws://192.168.1.161:9999/8f2a4c1e-9b77-4d21-8c3a-5e6f7a9c0d");
    state.url = url;
    state.url_ok = true;

    if (argc >= 4 && strcmp(argv[3], "nowifi") == 0) {
        // What the screen looks like before the console joined a network.
        state.url_ok = false;
        state.status.ip_text[0] = '\0';
        state.status.ip = 0;
        state.status.state = DglabNetState_Listening;
        state.status.peer_id[0] = '\0';
        state.status.paired = 0;
    }

    if (argc >= 4 && strcmp(argv[3], "stopped") == 0) {
        state.url_ok = false;
        state.status.state = DglabNetState_Idle;
        state.status.peer_id[0] = '\0';
        state.status.paired = 0;
    }

    dglabCanvasInit(&canvas, g_pixels, WIDTH, HEIGHT, WIDTH * 4);

    // PREVIEW_TTF=/path/to/font.ttf renders the localised screens with the real
    // glyph source; without it the bitmap font is used, which is ASCII only.
    {
        const char* ttf = getenv("PREVIEW_TTF");
        DglabGlyphSource* source = NULL;

        if (ttf) {
            FILE* font_file = fopen(ttf, "rb");

            if (font_file) {
                static uint8_t font_data[32 * 1024 * 1024];
                size_t size = fread(font_data, 1, sizeof(font_data), font_file);
                fclose(font_file);

                if (dglabTtfFontInit(font_data, size, 24.0f)) {
                    source = dglabTtfFontSource();
                    dglabStringsSetLanguage(DglabLanguage_ChineseSimplified);
                } else {
                    fprintf(stderr, "not a usable font: %s\n", ttf);
                }
            }
        }

        if (!source)
            source = dglabBitmapGlyphSource(&font);

        g_text = source;
    }

    if (argc >= 4 && strcmp(argv[3], "about") == 0) {
        DglabAboutState about;

        memset(&about, 0, sizeof(about));
        about.preference = DglabLanguage_Auto;
        about.resolved = DglabLanguage_ChineseSimplified;
        about.version = state.version;
        about.github_url = "https://github.com/livcm/DGLAB-NX";

        dglabAboutDraw(&canvas, g_text, &about);
    } else if (argc >= 4 && strcmp(argv[3], "menu") == 0) {
        DglabMenuState menu;

        memset(&menu, 0, sizeof(menu));
        menu.selected = DglabMenu_ItemMotion;
        menu.sysmodule_ok = true;

        dglabMenuDraw(&canvas, g_text, &menu);
    } else if (argc >= 4 && strcmp(argv[3], "motion") == 0) {
        DglabMotionScreenState motion;

        memset(&motion, 0, sizeof(motion));
        motion.left_connected = true;
        motion.right_connected = true;
        motion.moving_a = true;
        motion.level_a = 62;
        motion.frequency_a = 57;
        motion.moving_b = false;
        motion.level_b = 0;
        motion.frequency_b = 100;
        motion.channel_strength_a = 20;
        motion.channel_strength_b = 0;
        motion.link = dglabString(DglabString_LinkPaired);
        motion.link_tone = DglabCmdTone_Ok;
        motion.last_upload = "\u6ce2\u5f62 A  \u6b63\u5e38";
        motion.last_upload_tone = DglabCmdTone_Ok;
        motion.server_running = true;

        dglabMotionScreenDraw(&canvas, g_text, &motion);
    } else if (argc >= 4 && strcmp(argv[3], "advanced") == 0) {
        DglabMotionFeedConfig motion_config;
        DglabAdvancedState advanced;

        // A couple of values moved off the defaults, so the preview does not
        // just repeat the numbers from the source.
        dglabMotionSettingsDefault(&motion_config);
        dglabMotionSettingsStep(&motion_config, DglabMotionSetting_FrequencyFast, -4);
        dglabMotionSettingsStep(&motion_config, DglabMotionSetting_DeadzoneEnter, 3);

        memset(&advanced, 0, sizeof(advanced));
        advanced.config = &motion_config;
        advanced.selected = DglabMotionSetting_FrequencyFast;
        advanced.saved = true;

        dglabAdvancedDraw(&canvas, g_text, &advanced);
    } else {
        dglabScreenDraw(&canvas, &font, &state);
    }

    writeBmp(argv[2]);

    printf("wrote %s\n", argv[2]);

    return 0;
}
