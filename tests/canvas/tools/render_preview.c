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
//       -DDGLAB_TEST_LANG_DIR='"/path/to/DGLAB-NX/lang"' \
//       tests/canvas/tools/render_preview.c nro/source/ui/*.c nro/source/motion/*.c \
//       nro/source/util/json.c -o /tmp/preview -lm
//   /tmp/preview /tmp/font.bin /tmp/preview.bmp
//   /tmp/preview /tmp/font.bin /tmp/nowifi.bmp nowifi   (no LAN address yet)
//   /tmp/preview /tmp/font.bin /tmp/stopped.bmp stopped (server not started)
//   /tmp/preview /tmp/font.bin /tmp/menu.bmp menu       (the mode menu)
//   /tmp/preview /tmp/font.bin /tmp/motion.bmp motion   (the Joy-Con mode)
//   /tmp/preview /tmp/font.bin /tmp/touch.bmp touch     (the touch mode)
//   /tmp/preview /tmp/font.bin /tmp/touchdock.bmp touchdock  (a docked console)
//   /tmp/preview /tmp/font.bin /tmp/touchclamp.bmp touchclamp  (past both axis ends)
//   /tmp/preview /tmp/font.bin /tmp/touchfixed.bmp touchfixed  (density fixed)
//   /tmp/preview /tmp/font.bin /tmp/advanced.bmp advanced  (the motion parameters)
//   /tmp/preview /tmp/font.bin /tmp/advdensity.bmp advanceddensity  (the density rows)
//   /tmp/preview /tmp/font.bin /tmp/log.bmp log        (the sysmodule log page)
//   /tmp/preview /tmp/font.bin /tmp/aboutlow.bmp aboutlow  (the About page, end)
//   /tmp/preview /tmp/font.bin /tmp/dock.bmp menu dock (the docked 1080p frame)
//   sips -s format png /tmp/preview.bmp --out /tmp/preview.png
//
// PREVIEW_TTF=/path/to/font.ttf renders with the real glyph source instead of
// the bitmap font (the console's system font), and PREVIEW_LANG=en keeps the
// English strings with it: the console picks the face and the strings together.
// PREVIEW_THEME=light draws the light palette instead of the dark one.

#include <dglab/ui/screen.h>
#include <dglab/ui/advanced.h>
#include <dglab/ui/menu.h>
#include <dglab/ui/motion.h>
#include <dglab/ui/touch.h>
#include <dglab/nro/motion_settings.h>
#include <dglab/ui/about.h>
#include <dglab/ui/text_ttf.h>
#include <dglab/ui/language.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/theme.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../lang/lang_fixture.h"

// The handheld frame and the docked one (docs/nro-ui.md): the second is the same
// layout at 3/2, which is what a docked console draws. Pass `dock` after the
// page name to render that one.
#define WIDTH 1280
#define HEIGHT 720
#define DOCK_WIDTH 1920
#define DOCK_HEIGHT 1080

static uint8_t g_font_data[8192];
static uint8_t g_pixels[DOCK_WIDTH * DOCK_HEIGHT * 4];
static DglabFontSet g_fonts;
static int g_width = WIDTH;
static int g_height = HEIGHT;
static int g_scale_num = 1;
static int g_scale_den = 1;

static void writeBmp(const char* path)
{
    int row_size = (g_width * 3 + 3) & ~3;
    int data_size = row_size * g_height;
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
        memcpy(header + 18, &(int){ g_width }, 4);
        memcpy(header + 22, &(int){ g_height }, 4);
        memcpy(header + 26, &planes, 2);
        memcpy(header + 28, &bpp, 2);
        memcpy(header + 34, &data_size, 4);
    }

    fwrite(header, 1, sizeof(header), out);

    // BMP rows run bottom to top.
    for (int y = g_height - 1; y >= 0; y--) {
        memset(row, 0, (size_t)row_size);

        for (int x = 0; x < g_width; x++) {
            const uint8_t* pixel = g_pixels + ((size_t)y * (size_t)g_width + x) * 4;

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
        fprintf(stderr, "usage: render_preview <font.bin> <out.bmp> [normal|nowifi|stopped|"
                        "log|menu|motion|touch|touchdock|touchclamp|touchfixed|advanced|"
                        "advanceddensity|about|aboutlow] [dock]\n");
        return 2;
    }

    if (argc >= 5 && strcmp(argv[4], "dock") == 0) {
        g_width = DOCK_WIDTH;
        g_height = DOCK_HEIGHT;
        g_scale_num = 3;
        g_scale_den = 2;
    }

    // PREVIEW_THEME=light draws the pages with the light palette instead of the
    // dark one, so a theme can be checked next to the same screenshot the values
    // were measured from without a console in the loop.
    {
        const char* theme = getenv("PREVIEW_THEME");

        if (theme && strcmp(theme, "light") == 0)
            dglabThemeSet(&dglabThemeLight);
    }

    // The text comes from the files the NRO ships, not from the binary.
    {
        char message[512] = { 0 };

        if (!dglabTestLoadLanguages(message, sizeof(message))) {
            fprintf(stderr, "cannot load the language files: %s\n", message);
            return 5;
        }
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
    state.sysmodule_ok = true;
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

    dglabCanvasInit(&canvas, g_pixels, g_width, g_height, g_width * 4);
    dglabCanvasSetScale(&canvas, g_scale_num, g_scale_den);

    // PREVIEW_TTF=/path/to/font.ttf renders the localised screens with the real
    // glyph source, at the sizes the console's UI uses; without it the
    // bitmap font is used, which is ASCII only and has one size, so every size
    // ends up drawing with it.
    {
        const char* ttf = getenv("PREVIEW_TTF");
        bool loaded = false;

        if (ttf) {
            FILE* font_file = fopen(ttf, "rb");

            if (font_file) {
                static uint8_t font_data[32 * 1024 * 1024];
                size_t size = fread(font_data, 1, sizeof(font_data), font_file);
                fclose(font_file);

                DglabTtfFont* title = dglabTtfFontCreate(font_data, size, DGLAB_TEXT_TITLE,
                    g_scale_num, g_scale_den);
                DglabTtfFont* body = dglabTtfFontCreate(font_data, size, DGLAB_TEXT_BODY,
                    g_scale_num, g_scale_den);
                DglabTtfFont* value = dglabTtfFontCreate(font_data, size, DGLAB_TEXT_VALUE,
                    g_scale_num, g_scale_den);
                DglabTtfFont* note = dglabTtfFontCreate(font_data, size, DGLAB_TEXT_NOTE,
                    g_scale_num, g_scale_den);
                DglabTtfFont* icon = dglabTtfFontCreate(font_data, size, DGLAB_TEXT_ICON,
                    g_scale_num, g_scale_den);

                if (title && body && value && note && icon) {
                    const char* language = getenv("PREVIEW_LANG");

                    g_fonts.title = dglabTtfFontSource(title);
                    g_fonts.body = dglabTtfFontSource(body);
                    g_fonts.value = dglabTtfFontSource(value);
                    g_fonts.note = dglabTtfFontSource(note);
                    g_fonts.icon = dglabTtfFontSource(icon);
                    loaded = true;

                    dglabStringsSetLanguage((language && strcmp(language, "en") == 0)
                        ? DglabLanguage_English : DglabLanguage_ChineseSimplified);
                } else {
                    fprintf(stderr, "not a usable font: %s\n", ttf);
                }
            }
        }

        if (!loaded) {
            DglabGlyphSource* source = dglabBitmapGlyphSource(&font);

            g_fonts.title = source;
            g_fonts.body = source;
            g_fonts.value = source;
            g_fonts.note = source;
            g_fonts.icon = source;
        }
    }

    if (argc >= 4 && (strcmp(argv[3], "about") == 0 || strcmp(argv[3], "aboutlow") == 0)) {
        DglabAboutState about;

        memset(&about, 0, sizeof(about));
        about.preference = DglabLanguage_Auto;
        about.resolved = DglabLanguage_ChineseSimplified;
        // The two preference rows: the language one came from the loaded files,
        // the theme one from app.cfg. Auto shows what the console says, and the
        // preview is drawn with the palette it names - so "follow the system"
        // means the light theme in a light preview and the dark one otherwise.
        about.theme = DglabThemeMode_Auto;
        about.theme_system_is_dark = strcmp(getenv("PREVIEW_THEME") ? getenv("PREVIEW_THEME")
            : "", "light") != 0;
        // The three the page exists to show. The real ones come from the build
        // (VERSION and `git describe`, dglab/nro/version.h); this tool hardcodes
        // the look of them, as it does for the log lines and the url.
        about.app_version = "0.3.0";
        about.build_id = "2ac34ef";
        about.ipc_version.major = 0;
        about.ipc_version.minor = 2;
        about.sysmodule_ok = true;
        about.github_url = "https://github.com/livcm/DGLAB-NX";
        // The page is taller than the screen, so the theme row at the bottom is
        // only on screen once it is scrolled to the end - which is what the
        // `aboutlow` name renders (the page clamps the offset itself).
        about.offset = strcmp(argv[3], "aboutlow") == 0 ? 100000 : 0;

        dglabAboutDraw(&canvas, &g_fonts, &about);
    } else if (argc >= 4 && strcmp(argv[3], "menu") == 0) {
        DglabMenuState menu;

        memset(&menu, 0, sizeof(menu));
        menu.selected = DglabMenu_ItemMotion;
        menu.sysmodule_ok = true;

        dglabMenuDraw(&canvas, &g_fonts, &menu);
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
        motion.link = dglabString(DglabString_StateConnected);
        motion.link_tone = DglabCmdTone_Ok;
        motion.last_upload = "\u6ce2\u5f62 A  \u6b63\u5e38";
        motion.last_upload_tone = DglabCmdTone_Ok;
        motion.server_running = true;

        dglabMotionScreenDraw(&canvas, &g_fonts, &motion);
    } else if (argc >= 4 && (strcmp(argv[3], "touch") == 0 ||
                                strcmp(argv[3], "touchdock") == 0 ||
                                strcmp(argv[3], "touchclamp") == 0 ||
                                strcmp(argv[3], "touchfixed") == 0)) {
        DglabTouchScreenState touch;
        bool docked = strcmp(argv[3], "touchdock") == 0;
        bool clamp = strcmp(argv[3], "touchclamp") == 0;
        bool fixed = strcmp(argv[3], "touchfixed") == 0;

        memset(&touch, 0, sizeof(touch));
        touch.held_a = !docked;
        touch.x_a = clamp ? 0 : 300;
        touch.y_a = 260;
        touch.level_a = clamp ? 100 : 69;
        touch.frequency_a = fixed ? 65 : (clamp ? 100 : 52);
        touch.held_b = !docked;
        touch.x_b = clamp ? DGLAB_TOUCH_PANEL_WIDTH - 1 : 900;
        touch.y_b = 420;
        touch.level_b = clamp ? 0 : 41;
        touch.frequency_b = fixed ? 65 : (clamp ? 30 : 60);
        touch.docked = docked;
        // With the density fixed the horizontal axis is not the input any more,
        // so the field is drawn without it.
        touch.density_fixed = fixed;
        touch.channel_strength_a = 20;
        touch.channel_strength_b = 0;
        touch.link = dglabString(DglabString_StateConnected);
        touch.link_tone = DglabCmdTone_Ok;
        touch.last_upload = "\u6ce2\u5f62 A  \u6b63\u5e38";
        touch.last_upload_tone = DglabCmdTone_Ok;
        touch.server_running = true;
        touch.sysmodule_ok = true;

        dglabTouchScreenDraw(&canvas, &g_fonts, &touch);
    } else if (argc >= 4 && (strcmp(argv[3], "advanced") == 0 ||
                                strcmp(argv[3], "advanceddensity") == 0)) {
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

        // The second name turns the density switch on and puts the cursor on the
        // row below it, so both new rows - the one that shows a word and the one
        // it makes meaningful - are on screen together.
        if (strcmp(argv[3], "advanceddensity") == 0) {
            motion_config.density_fixed = true;
            advanced.selected = DglabMotionSetting_FrequencyFixed;
        }

        dglabAdvancedDraw(&canvas, &g_fonts, &advanced);
    } else if (argc >= 4 && strcmp(argv[3], "log") == 0) {
        // The sysmodule log page: what Y opens on the socket page.
        state.log_open = true;
        state.log_offset = 0;

        dglabScreenDraw(&canvas, &g_fonts, &state);
    } else {
        dglabScreenDraw(&canvas, &g_fonts, &state);
    }

    writeBmp(argv[2]);

    printf("wrote %s\n", argv[2]);

    return 0;
}
