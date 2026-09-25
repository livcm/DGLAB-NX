// DGLAB-NX front end.
//
// Polls the sysmodule over IPC, feeds the result into the screen layout, and
// turns button presses into IPC commands. The drawing itself lives in
// dglab/ui/screen.c, which stays free of libnx so it can be rendered and
// checked on a PC.
//
// The NRO never owns a DG-LAB connection: the sysmodule is the only owner, and
// everything here goes through the IPC surface in dglab/ipc.h.

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include <dglab/ipc.h>
#include <dglab/ipc_poc.h>
// This NRO's own release version and build stamp. Not the IPC version: that one
// is DGLAB_IPC_PROTOCOL_VERSION in the header above, and the two are shown side
// by side on the About page.
#include <dglab/nro/version.h>
#include <dglab/platform/langfiles.h>
#include <dglab/nro/auto_sleep.h>
#include <dglab/nro/joycon.h>
#include <dglab/platform/font.h>
#include <dglab/ui/about.h>
#include <dglab/ui/ble.h>
#include <dglab/ui/language.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/text.h>
#include <dglab/nro/motion_feed.h>
#include <dglab/nro/motion_settings.h>
#include <dglab/nro/touch_feed.h>
#include <dglab/nro/touch_panel.h>
#include <dglab/nro/ble_poc_view.h>
#include <dglab/platform/framebuffer.h>
#include <dglab/platform/touchscreen.h>
#include <dglab/ui/advanced.h>
#include <dglab/ui/menu.h>
#include <dglab/ui/motion.h>
#include <dglab/ui/page.h>
#include <dglab/ui/screen.h>
#include <dglab/ui/settings.h>
#include <dglab/ui/theme.h>
#include <dglab/ui/touch.h>

#define LOG_POLL_ROUNDS 2

// The log is the only thing here that arrives continuously, and the file it is
// mirrored to does not care about the display refresh rate: the log IPC calls
// are spread over a few frames instead of running 60 times a second.
#define LOG_POLL_INTERVAL_FRAMES 3

// Everything the front end writes lives under one directory on the SD card,
// split by what it is: the language files it reads, the settings it writes and
// the logs it mirrors (docs/nro-ui.md).
#define DATA_DIR "sdmc:/switch/DGLAB-NX"
#define CONFIG_DIR DATA_DIR "/config"
#define LOG_DIR DATA_DIR "/logs"
// The sysmodule log is mirrored to the SD card, so a test run can be reported
// back as a file instead of as a photo of the screen.
#define LOG_FILE_PATH LOG_DIR "/dglab-net.log"
// The motion parameters, so a tuning session does not start over every reboot.
#define MOTION_CONFIG_PATH CONFIG_DIR "/motion.cfg"
// The app level settings (the UI language and the colour theme).
#define APP_CONFIG_PATH CONFIG_DIR "/app.cfg"

typedef enum {
    DglabMenuResult_Exit = 0,
    DglabMenuResult_Socket,
    DglabMenuResult_Ble,
    DglabMenuResult_Motion,
    DglabMenuResult_Touch,
    DglabMenuResult_Advanced,
    DglabMenuResult_About,
    DglabMenuResult_BlePoc,
} DglabMenuResult;

// The four sizes every screen draws with: the system shared font at the sizes
// the console's own UI uses (docs/nro-ui.md), or libnx's bitmap font when that
// cannot be loaded - which only has ASCII, so Chinese text would show as gaps.
static DglabFontSet g_fonts;
static DglabFontSet g_bitmap_fonts;
static DglabAppSettings g_settings;
static DglabLanguage g_language = DglabLanguage_English;
// What the console says about its own theme, read when the palette is applied
// (startup and every rebuilt display) rather than every frame: an applet is
// suspended while the user is in the system settings, so there is no theme
// change to notice in between. `g_system_theme_ok` is false when set:sys did not
// answer at all, which makes Auto mean dark - the same fallback a console that
// cannot be asked gets (docs/nro-ui.md).
static bool g_system_is_dark = true;
static bool g_system_theme_ok;
// Whether this process holds set:sys at all. When it does not, the query is not
// attempted - the fallback is decided without touching a service we never took.
static bool g_set_sys_ready;

// Bumped whenever the display is rebuilt: a new framebuffer is blank, and the
// fonts are rasterised anew for it (the docked frame is 1.5x the handheld one),
// so every screen has to draw again even if nothing else about it changed.
static u32 g_display_generation;

static void appSettingsSave(void)
{
    char text[128];
    FILE* file;

    dglabAppSettingsSerialize(&g_settings, text, sizeof(text));

    file = fopen(APP_CONFIG_PATH, "w");

    if (file == NULL)
        return;

    fputs(text, file);
    fclose(file);
}

static void appSettingsLoad(void)
{
    char text[128];
    size_t size;
    FILE* file = fopen(APP_CONFIG_PATH, "r");

    dglabAppSettingsDefault(&g_settings);

    if (file == NULL)
        return;

    size = fread(text, 1, sizeof(text) - 1, file);
    text[size] = '\0';
    fclose(file);

    // A file that came from an older build (a `language=` line and nothing else)
    // keeps that language and takes the defaults for everything else.
    dglabAppSettingsParse(text, &g_settings);
}

// Resolves the preference, tells the string tables, and loads the matching face.
static void appLanguageApply(void)
{
    g_language = dglabLanguageResolve(g_settings.language, dglabFontSystemIsChinese());
    dglabStringsSetLanguage(g_language);

    {
        const DglabFontSet* fonts =
            dglabFontOpen(g_language == DglabLanguage_ChineseSimplified);

        if (fonts != NULL) {
            g_fonts = *fonts;
            return;
        }
    }

    // The bitmap font has one size, so the fallback draws every size with it.
    g_bitmap_fonts.title = dglabBitmapGlyphSource(dglabFramebufferFont());
    g_bitmap_fonts.body = g_bitmap_fonts.title;
    g_bitmap_fonts.value = g_bitmap_fonts.title;
    g_bitmap_fonts.note = g_bitmap_fonts.title;
    g_bitmap_fonts.icon = g_bitmap_fonts.title;
    g_fonts = g_bitmap_fonts;
}

// The one place the palette is chosen: the console is asked what its theme is
// (once per call - startup and a rebuilt display), and the preference decides
// what the screens draw with. A console that does not answer leaves Auto on the
// dark palette, which the log panel says out loud.
static void appThemeApply(void)
{
    ColorSetId color_set = ColorSetId_Dark;
    bool answered = g_set_sys_ready && R_SUCCEEDED(setsysGetColorSetId(&color_set));

    g_system_theme_ok = answered;
    g_system_is_dark = !answered || color_set != ColorSetId_Light;

    dglabThemeSet(dglabThemeResolve(g_settings.theme, g_system_is_dark));
}

// ---------------------------------------------------------------------------
// The display
// ---------------------------------------------------------------------------

// Gives the screen up before a view that needs the console (consoleInit takes
// the same window), and takes it back afterwards. The shared font mapping is
// released as well: it is the NRO's largest allocation and the console view has
// its own bitmap font.
static void appDisplaySuspend(void)
{
    dglabFontClose();
    dglabFramebufferClose();
}

// Builds the framebuffer again, at whatever resolution the console is running
// at now, and re-rasterises the fonts for it. Every screen has to redraw after
// this, which g_display_generation tells them.
//
// Without the appLanguageApply() the font objects would still point at the
// shared font mapping appDisplaySuspend() released: the glyph caches would draw
// what they already held and every new character would come out blank. Leaving
// the BLE PoC console used to do exactly that, and only a restart recovered.
// The theme is applied again here too, which is the other half of "a rebuilt
// display draws everything from scratch".
static bool appDisplayReopen(void)
{
    if (!dglabFramebufferOpen())
        return false;

    appLanguageApply();
    appThemeApply();
    g_display_generation++;

    return true;
}

// The console tells us when it is docked or undocked; the callback only records
// it, and the main loop rebuilds the display between frames.
static volatile bool g_display_mode_dirty;

static void appletHookCallback(AppletHookType hook, void* param)
{
    (void)param;

    if (hook == AppletHookType_OnOperationMode)
        g_display_mode_dirty = true;
}

static char g_log_lines[DGLAB_SCREEN_LOG_LINES][DGLAB_SCREEN_LOG_LINE_LEN];
static const char* g_log_pointers[DGLAB_SCREEN_LOG_LINES];
static int g_log_filled;
static u32 g_log_cursor;
static char g_partial[256];
static size_t g_partial_len;
// Bumped for every line the log ring takes in, so the view can tell that the
// panel changed even when the ring is full and the line count stays the same.
static u32 g_log_generation;
// Text and tone for the "last cmd" line. Every command the buttons send answers
// with a Result, and this end used to throw them away: pressing a test key with
// no App bound, or with the server stopped, looked exactly like pressing it
// successfully. The line is the only place a failed press can surface, because
// the sysmodule has nothing to log when a request never left the console. It is
// built here rather than in the screen code, which stays platform independent.
static char g_last_command[40];
static u32 g_last_command_tone;
// The value the test buttons use is the raw channel strength, the same number
// the App shows: 0..100 (the official documentation only allows more than 100
// for special cases), one step per press and no wrap around. Each channel keeps
// its own value, and both start at 0: nothing comes out until the user dials a
// channel up.
#define TEST_STRENGTH_MIN 0u
#define TEST_STRENGTH_MAX 100u
#define TEST_STRENGTH_STEP 1u

// Channel numbers on the wire, see DglabNetSendRequest::channel.
#define TEST_CHANNEL_A 1u
#define TEST_CHANNEL_B 2u

// Holding a direction starts walking the value only after a deliberate hold, and
// then at a usable rate. The first version repeated after six frames (~100ms at
// the display refresh), which is exactly how long a normal press lasts: a firm
// tap was read as a hold and stepped twice. 400ms is past any tap, and the walk
// then runs at 10 steps/s.
#define TEST_STRENGTH_HOLD_NS (400ull * 1000000ull)
#define TEST_STRENGTH_REPEAT_NS (100ull * 1000000ull)
static u32 g_test_strength_a = TEST_STRENGTH_MIN;
static u32 g_test_strength_b = TEST_STRENGTH_MIN;
static u64 g_strength_hold_started_ns; // 0 while no direction is held
static u64 g_strength_last_repeat_ns;
static FILE* g_log_file;
// ---------------------------------------------------------------------------
// Log ring
// ---------------------------------------------------------------------------

static void logPushLine(const char* line)
{
    size_t len = strlen(line);

    if (DGLAB_SCREEN_LOG_LINES > 1)
        memmove(g_log_lines[0], g_log_lines[1],
            sizeof(g_log_lines[0]) * (DGLAB_SCREEN_LOG_LINES - 1));

    if (len >= DGLAB_SCREEN_LOG_LINE_LEN)
        len = DGLAB_SCREEN_LOG_LINE_LEN - 1;

    memcpy(g_log_lines[DGLAB_SCREEN_LOG_LINES - 1], line, len);
    g_log_lines[DGLAB_SCREEN_LOG_LINES - 1][len] = '\0';

    g_log_generation++;

    if (g_log_filled < DGLAB_SCREEN_LOG_LINES)
        g_log_filled++;

    if (g_log_file != NULL) {
        fprintf(g_log_file, "%s\n", line);
        fflush(g_log_file);
    }
}

// ---------------------------------------------------------------------------
// The console's automatic sleep timer
// ---------------------------------------------------------------------------

// One line per change of the console's flag, in the same panel the sysmodule's
// log lines go to. The reason this exists at all is in docs/dglab-socket.md
// ("睡眠与唤醒"): a sleep with the server's sockets open hangs the console, and
// the sysmodule is never told that one is coming.
static void appAutoSleepLog(const char* verb, DglabAutoSleepEvent event, Result rc)
{
    char line[DGLAB_SCREEN_LOG_LINE_LEN];

    switch (event) {
        case DglabAutoSleepEvent_Suppressed:
            logPushLine("auto sleep: off while the server runs");
            break;

        case DglabAutoSleepEvent_AlreadyOff:
            logPushLine("auto sleep: already off, left alone");
            break;

        case DglabAutoSleepEvent_Restored:
            logPushLine("auto sleep: restored");
            break;

        case DglabAutoSleepEvent_Failed:
            snprintf(line, sizeof(line), "auto sleep: %s rc=0x%08X", verb, (unsigned)rc);
            logPushLine(line);
            break;

        default:
            break;
    }
}

// Follows what the pages poll for the server: only a change does anything, so
// calling this every frame costs nothing. Both pages that can start the server
// call it, which is what keeps "the server is up" and "automatic sleep is off"
// from drifting apart in the direction that hangs the console.
static void appAutoSleepFollow(bool running)
{
    Result rc = 0;
    DglabAutoSleepEvent event = dglabAutoSleepFollowServer(running, &rc);

    appAutoSleepLog(running ? "disable" : "restore", event, rc);
}

// Called once on the way out, before the log file is closed.
static void appAutoSleepRestore(void)
{
    Result rc = 0;
    DglabAutoSleepEvent event = dglabAutoSleepRestore(&rc);

    appAutoSleepLog("restore", event, rc);
}

// libnx's default init already mounts sdmc, so a failure to open the file is
// reported on screen rather than treated as fatal.
static void logFileOpen(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(DATA_DIR, 0777);
    mkdir(CONFIG_DIR, 0777);
    mkdir(LOG_DIR, 0777);

    g_log_file = fopen(LOG_FILE_PATH, "w");

    if (g_log_file == NULL)
        g_log_file = fopen("sdmc:/dglab-net.log", "w");
}

static void logAppend(const char* text, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        char c = text[i];

        if (c == '\n') {
            g_partial[g_partial_len] = '\0';
            logPushLine(g_partial);
            g_partial_len = 0;
            continue;
        }

        if (c == '\r' || c == '\0')
            continue;

        if (g_partial_len + 1 < sizeof(g_partial))
            g_partial[g_partial_len++] = c;
    }
}

static void logPoll(Service* dglab)
{
    for (int i = 0; i < LOG_POLL_ROUNDS; i++) {
        DglabNetLogRequest request = { 0 };
        DglabNetLogChunk chunk;
        Result rc;

        request.cursor = g_log_cursor;
        memset(&chunk, 0, sizeof(chunk));

        rc = serviceDispatchInOut(dglab, DGLAB_IPC_CMD_NET_LOG, request, chunk);

        if (R_FAILED(rc))
            return;

        if (chunk.size) {
            u32 size = chunk.size;

            if (size > sizeof(chunk.text))
                size = sizeof(chunk.text);

            logAppend(chunk.text, size);
        }

        if (chunk.next_cursor == g_log_cursor || chunk.size == 0)
            return;

        g_log_cursor = chunk.next_cursor;
    }
}

// The screen takes a plain array of lines, in the order they should be drawn.
static void buildLogPointers(void)
{
    for (int i = 0; i < g_log_filled; i++)
        g_log_pointers[i] = g_log_lines[DGLAB_SCREEN_LOG_LINES - g_log_filled + i];
}

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------

// Whether the sysmodule is still there, which is what every page's title bar
// shows on its right hand side. The menu polled this first, and only the pages
// that talk to the sysmodule on their own (socket, motion) used to have any
// idea; the front end now asks from one place, so the line means the same thing
// whichever page is up. One PING a second is enough for a service this NRO
// cannot run without, and it keeps the IPC traffic away from the frame rate.
#define APP_PING_FRAMES 60
static u32 g_ping_frames;
static bool g_sysmodule_ok = true;

static bool appSysmoduleOk(Service* dglab)
{
    if (g_ping_frames++ % APP_PING_FRAMES == 0) {
        u32 pong = 0;

        g_sysmodule_ok = R_SUCCEEDED(serviceDispatchOut(dglab, DGLAB_IPC_CMD_PING, pong)) &&
            pong == DGLAB_IPC_PING_MAGIC;
    }

    return g_sysmodule_ok;
}

// Turns the Result of a command into the single line the screen shows. The
// failures the buttons can actually cause are named; anything else is shown as a
// hex code rather than guessed at. Keep the result under 21 characters: that is
// what the value column of the panel holds.
static void noteCommand(const char* what, Result result, const char* success_note)
{
    const char* outcome = NULL;

    if (R_SUCCEEDED(result)) {
        if (success_note)
            snprintf(g_last_command, sizeof(g_last_command), "%s  %s (%s)", what,
                dglabString(DglabString_CmdOk), success_note);
        else
            snprintf(g_last_command, sizeof(g_last_command), "%s  %s", what,
                dglabString(DglabString_CmdOk));
    } else {
        if (R_DESCRIPTION(result) == LibnxError_NotFound)
            outcome = dglabString(DglabString_CmdNoApp);
        else if (R_DESCRIPTION(result) == LibnxError_BadInput)
            outcome = dglabString(DglabString_CmdRejected);
        else if (R_DESCRIPTION(result) == LibnxError_IoError)
            outcome = dglabString(DglabString_CmdSocketError);

        if (outcome)
            snprintf(g_last_command, sizeof(g_last_command), "%s  %s", what, outcome);
        else
            snprintf(g_last_command, sizeof(g_last_command), "%s  0x%08X", what, (unsigned)result);
    }

    if (R_FAILED(result))
        g_last_command_tone = DglabCmdTone_Error;
    else if (success_note)
        g_last_command_tone = DglabCmdTone_Warn;
    else
        g_last_command_tone = DglabCmdTone_Ok;
}

static Result sendTestCommand(Service* dglab, u32 command, u32 channel, u32 value)
{
    DglabNetSendRequest request = { 0 };

    request.command = command;
    request.channel = channel;
    request.value = value;

    return serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_SEND, request);
}

// Queues one second of test waveform through the streaming path: 48 slots of
// 25ms each, at the app side frequency of 100ms, with the waveform at full
// strength so the channel strength alone decides how strong it feels. The
// waveform goes out on one channel; Replace mode makes every press start a fresh
// gesture rather than queueing behind the previous one.
static Result sendTestWaveform(Service* dglab, u32 channel)
{
    DglabNetWaveformRequest request;

    memset(&request, 0, sizeof(request));
    request.channel = channel;
    request.mode = DglabNetWaveform_Replace;
    request.slot_count = DGLAB_NET_WAVEFORM_MAX_SLOTS;

    for (u32 i = 0; i < request.slot_count; i++) {
        request.slots[i].frequency_ms = 100;
        request.slots[i].strength = 100;
    }

    return serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_WAVEFORM, request);
}

// Fires one channel: the waveform plus that channel's strength, so the button is
// self contained even if the App reconnected since the strength last changed.
static Result testChannel(Service* dglab, u32 channel, u32 strength)
{
    Result rc = sendTestWaveform(dglab, channel);

    if (R_FAILED(rc))
        return rc;

    return sendTestCommand(dglab, DglabNetCommand_SetStrength, channel, strength);
}

// Steps one channel's strength and sends it straight away - there is no separate
// "send strength" button. Clamped instead of wrapping: at 0 a decrease does
// nothing, at 100 an increase does nothing, and a clamped step sends nothing (so
// there is nothing to report either).
static bool adjustStrength(Service* dglab, u32 channel, u32* value, int delta, Result* out)
{
    int next = (int)*value + delta;

    if (next < (int)TEST_STRENGTH_MIN)
        next = (int)TEST_STRENGTH_MIN;

    if (next > (int)TEST_STRENGTH_MAX)
        next = (int)TEST_STRENGTH_MAX;

    if ((u32)next == *value)
        return false;

    *value = (u32)next;
    *out = sendTestCommand(dglab, DglabNetCommand_SetStrength, channel, *value);

    return true;
}

// The D-pad is the mixer: up and down dial channel A, left and right dial channel
// B, one step per press and, at a slower rate, while held. The socket and motion
// pages share it, because they show the same two strengths.
static void adjustStrengthFromDirections(Service* dglab, u64 buttons)
{
    Result rc;

    if (buttons & HidNpadButton_Up &&
        adjustStrength(dglab, TEST_CHANNEL_A, &g_test_strength_a, (int)TEST_STRENGTH_STEP, &rc))
        noteCommand(dglabString(DglabString_CmdUpA), rc, NULL);

    if (buttons & HidNpadButton_Down &&
        adjustStrength(dglab, TEST_CHANNEL_A, &g_test_strength_a, -(int)TEST_STRENGTH_STEP, &rc))
        noteCommand(dglabString(DglabString_CmdDownA), rc, NULL);

    if (buttons & HidNpadButton_Right &&
        adjustStrength(dglab, TEST_CHANNEL_B, &g_test_strength_b, (int)TEST_STRENGTH_STEP, &rc))
        noteCommand(dglabString(DglabString_CmdUpB), rc, NULL);

    if (buttons & HidNpadButton_Left &&
        adjustStrength(dglab, TEST_CHANNEL_B, &g_test_strength_b, -(int)TEST_STRENGTH_STEP, &rc))
        noteCommand(dglabString(DglabString_CmdDownB), rc, NULL);
}

// Repeats a held direction: nothing for the first TEST_STRENGTH_HOLD_NS, then one
// step every TEST_STRENGTH_REPEAT_NS. The press itself is handled from `down`, so
// a short tap always changes the value exactly once.
static void repeatStrengthFromDirections(Service* dglab, u64 held, u64 now_ns)
{
    if (!(held & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Right |
            HidNpadButton_Left))) {
        g_strength_hold_started_ns = 0;
        return;
    }

    if (g_strength_hold_started_ns == 0) {
        // 0 doubles as "not held", so a clock that reads 0 still starts a hold.
        g_strength_hold_started_ns = now_ns ? now_ns : 1u;
        g_strength_last_repeat_ns = 0;
        return;
    }

    if (now_ns - g_strength_hold_started_ns < TEST_STRENGTH_HOLD_NS)
        return;

    if (g_strength_last_repeat_ns != 0 &&
        now_ns - g_strength_last_repeat_ns < TEST_STRENGTH_REPEAT_NS)
        return;

    g_strength_last_repeat_ns = now_ns;

    adjustStrengthFromDirections(dglab, held);
}

// Starts or stops the server: what A does on the socket page and on the motion
// page, where the bottom bar's own label follows this decision. `running` is the
// state the calling page polled for this frame and drew - the same value the label
// came from - so the key can never act on a staler belief than the one on screen.
// (It used to read one global that only the socket page updated, which made A a
// no-op on the motion page: that page's own label said "stop" while the global
// still said "stopped", and the sysmodule answers NET_START on a running server
// with success and does nothing.)
static void toggleServer(Service* dglab, bool running)
{
    if (running) {
        noteCommand(dglabString(DglabString_ActionStop),
            serviceDispatch(dglab, DGLAB_IPC_CMD_NET_STOP), NULL);
    } else {
        DglabNetStartRequest request = { 0 };

        noteCommand(dglabString(DglabString_ActionStart),
            serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_START, request), NULL);
    }
}

// The two shortcuts both pages share: ZL and ZR fire one channel each (a
// waveform without a strength does nothing, and the strength alone is just as
// silent, so each button sends both), X clears what the last test left playing.
static void testChannelButtons(Service* dglab, u64 down)
{
    if (down & HidNpadButton_ZL)
        noteCommand(dglabString(DglabString_CmdTestA),
            testChannel(dglab, TEST_CHANNEL_A, g_test_strength_a),
            g_test_strength_a ? NULL : dglabString(DglabString_CmdChannelZeroA));

    if (down & HidNpadButton_ZR)
        noteCommand(dglabString(DglabString_CmdTestB),
            testChannel(dglab, TEST_CHANNEL_B, g_test_strength_b),
            g_test_strength_b ? NULL : dglabString(DglabString_CmdChannelZeroB));

    if (down & HidNpadButton_X)
        noteCommand(dglabString(DglabString_CmdClear),
            sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

// Everything the screen draws, so the loop can tell whether redrawing is worth
// it. A full 1280x720 frame plus the QR code is expensive, and the panel only
// changes when one of these does.
typedef struct {
    DglabNetStatus status;
    bool status_ok;
    bool sysmodule_ok;
    bool url_ok;
    char url[DGLAB_NET_QR_MAX];
    u32 test_strength_a;
    u32 test_strength_b;
    char last_command[sizeof(g_last_command)];
    u32 last_command_tone;
    // Swaps the row of warning text under the server row, so it belongs in the
    // redraw decision like any other thing the page draws (nro/AGENTS.md).
    bool auto_sleep_suppressed;
    int log_count;
    u32 log_generation;
    bool log_open;
    int log_offset;
    // The display was rebuilt: the frame on screen is gone, so the snapshot the
    // view last drew no longer describes what is up there.
    u32 display_generation;
} DglabScreenSnapshot;

static void snapshotFromState(DglabScreenSnapshot* out, const DglabScreenState* state)
{
    memset(out, 0, sizeof(*out));

    out->status = state->status;
    out->status_ok = state->status_ok;
    out->sysmodule_ok = state->sysmodule_ok;
    out->url_ok = state->url_ok;
    out->test_strength_a = state->test_strength_a;
    out->test_strength_b = state->test_strength_b;
    out->last_command_tone = state->last_command_tone;
    out->auto_sleep_suppressed = state->auto_sleep_suppressed;
    out->log_count = state->log_count;
    out->log_generation = g_log_generation;
    out->log_open = state->log_open;
    out->log_offset = state->log_offset;
    out->display_generation = g_display_generation;

    snprintf(out->last_command, sizeof(out->last_command), "%s",
        state->last_command ? state->last_command : "");

    if (state->url)
        snprintf(out->url, sizeof(out->url), "%s", state->url);
}

// How far the log page can scroll: the lines that do not fit the view.
static int logMaxOffset(int log_count)
{
    int view = DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP;
    int content = log_count * DGLAB_SCREEN_LOG_PITCH;

    return content > view ? content - view : 0;
}

// The log page scrolls one line per press and then repeats while a direction is
// held. One line is DGLAB_SCREEN_LOG_PITCH pixels - a press used to move a single
// pixel, which read as "nothing happened, and then it crawls" - and the repeat
// has its own timing, faster than the strength keys: a log is something you fly
// through, and the whole page is only a screenful away from either end.
#define LOG_SCROLL_HOLD_NS (300ull * 1000000ull)
#define LOG_SCROLL_REPEAT_NS (50ull * 1000000ull)
static u64 g_log_hold_started_ns;
static u64 g_log_last_repeat_ns;

static int logScrollFromDirections(int offset, int max_offset, u64 down, u64 held, u64 now_ns)
{
    int lines = 0;

    if (down & HidNpadButton_Up)
        lines -= 1;

    if (down & HidNpadButton_Down)
        lines += 1;

    if (lines == 0) {
        if (!(held & (HidNpadButton_Up | HidNpadButton_Down))) {
            g_log_hold_started_ns = 0;
            return offset;
        }

        if (g_log_hold_started_ns == 0) {
            g_log_hold_started_ns = now_ns ? now_ns : 1u;
            g_log_last_repeat_ns = 0;
            return offset;
        }

        if (now_ns - g_log_hold_started_ns < LOG_SCROLL_HOLD_NS ||
            (g_log_last_repeat_ns != 0 &&
                now_ns - g_log_last_repeat_ns < LOG_SCROLL_REPEAT_NS))
            return offset;

        g_log_last_repeat_ns = now_ns;
        lines = (held & HidNpadButton_Down) ? 1 : -1;
    } else {
        g_log_hold_started_ns = now_ns ? now_ns : 1u;
        g_log_last_repeat_ns = 0;
    }

    offset += lines * DGLAB_SCREEN_LOG_PITCH;

    return dglabListScrollClamp(offset, max_offset);
}

// The socket page and its log page. B goes back to the menu; every action here
// is a shortcut key, so there is nothing to focus and nothing to navigate.
static void runSocketView(Service* dglab, PadState* pad)
{
    DglabScreenSnapshot snapshot;
    bool have_snapshot = false;
    // What the A button means here: whether the server held a socket as of this
    // frame's status poll, which is also what the bottom bar's label says.
    bool server_running = false;
    char url[DGLAB_NET_QR_MAX];
    bool url_ok = false;
    // The log page is part of this view: Y opens it and closes it again, and it
    // opens on the newest line, which is what a log is read for.
    bool log_open = false;
    int log_offset = 0;
    u32 frame = 0;

    url[0] = '\0';

    while (appletMainLoop()) {
        DglabScreenState state;
        DglabNetQrChunk chunk;
        u64 down;
        u64 held;

        padUpdate(pad);
        down = padGetButtonsDown(pad);
        held = padGetButtons(pad);

        memset(&state, 0, sizeof(state));
        memset(&chunk, 0, sizeof(chunk));

        state.url = url;
        state.url_ok = url_ok;
        state.sysmodule_ok = appSysmoduleOk(dglab);

        state.status_ok =
            R_SUCCEEDED(serviceDispatchOut(dglab, DGLAB_IPC_CMD_NET_STATUS, state.status));

        // The log is the one thing that arrives continuously, and the file it is
        // mirrored to does not care about 60Hz: polling every few frames keeps
        // the IPC traffic down without a visible delay.
        if (++frame % LOG_POLL_INTERVAL_FRAMES == 1) {
            logPoll(dglab);
            buildLogPointers();
        }

        state.log_lines = g_log_pointers;
        state.log_count = g_log_filled;

        if (R_SUCCEEDED(serviceDispatchOut(dglab, DGLAB_IPC_CMD_NET_QR, chunk))) {
            url_ok = true;
            snprintf(url, sizeof(url), "%s", chunk.text);
        } else {
            // Without a LAN address there is no QR code to show.
            url_ok = false;
            url[0] = '\0';
        }

        state.url_ok = url_ok;
        server_running = state.status_ok &&
            (state.status.state == DglabNetState_Listening ||
                state.status.state == DglabNetState_Paired);
        appAutoSleepFollow(server_running);

        if (down & HidNpadButton_B)
            return;

        if (down & HidNpadButton_Y) {
            log_open = !log_open;

            if (log_open)
                log_offset = logMaxOffset(g_log_filled);
        } else if (log_open) {
            log_offset = logScrollFromDirections(log_offset, logMaxOffset(g_log_filled), down, held,
                armTicksToNs(armGetSystemTick()));
        } else {
            if (down & HidNpadButton_A)
                toggleServer(dglab, server_running);

            testChannelButtons(dglab, down);
            adjustStrengthFromDirections(dglab, down);
            repeatStrengthFromDirections(dglab, held, armTicksToNs(armGetSystemTick()));
        }

        state.log_open = log_open;
        state.log_offset = log_offset;

        // Read after the buttons are handled, so a press shows up in the same
        // frame it happened.
        state.test_strength_a = g_test_strength_a;
        state.test_strength_b = g_test_strength_b;
        state.last_command = g_last_command;
        state.last_command_tone = g_last_command_tone;
        // Which of the two warning lines the server row carries. It follows the
        // flag this frame's poll set, which is one frame behind a press - the
        // same lag the status row above already has.
        state.auto_sleep_suppressed = dglabAutoSleepActive();

        DglabScreenSnapshot candidate;

        snapshotFromState(&candidate, &state);

        if (!have_snapshot || memcmp(&candidate, &snapshot, sizeof(candidate)) != 0) {
            DglabCanvas canvas;

            if (dglabFramebufferBegin(&canvas)) {
                dglabScreenDraw(&canvas, &g_fonts, &state);
                dglabFramebufferEnd();

                snapshot = candidate;
                have_snapshot = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The menu
// ---------------------------------------------------------------------------

// Kept across visits, so leaving a mode comes back to the same entry.
static unsigned g_menu_selected = DglabMenu_ItemSocket;

static DglabMenuResult runMenuView(Service* dglab, PadState* pad)
{
    DglabMenuState state;
    unsigned drawn_selected = 0;
    bool drawn_liveness = false;
    u32 drawn_generation = 0;
    bool have_drawn = false;

    memset(&state, 0, sizeof(state));
    state.sysmodule_ok = true;

    while (appletMainLoop()) {
        DglabCanvas canvas;
        u64 down;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        // B quits from the menu, the way the console's own home menu does; +
        // stays as the shortcut it always was.
        if (down & HidNpadButton_B)
            return DglabMenuResult_Exit;

        if (down & HidNpadButton_Up)
            g_menu_selected = dglabMenuMove(g_menu_selected, -1);

        if (down & HidNpadButton_Down)
            g_menu_selected = dglabMenuMove(g_menu_selected, 1);

        if (down & HidNpadButton_A) {
            switch (g_menu_selected) {
                case DglabMenu_ItemBle: return DglabMenuResult_Ble;
                case DglabMenu_ItemMotion: return DglabMenuResult_Motion;
                case DglabMenu_ItemTouch: return DglabMenuResult_Touch;
                case DglabMenu_ItemAdvanced: return DglabMenuResult_Advanced;
                case DglabMenu_ItemAbout: return DglabMenuResult_About;
                case DglabMenu_ItemBlePoc: return DglabMenuResult_BlePoc;
                default: return DglabMenuResult_Socket;
            }
        }

        state.sysmodule_ok = appSysmoduleOk(dglab);

        state.selected = g_menu_selected;

        if (have_drawn && state.selected == drawn_selected &&
            state.sysmodule_ok == drawn_liveness && drawn_generation == g_display_generation)
            continue;

        if (dglabFramebufferBegin(&canvas)) {
            dglabMenuDraw(&canvas, &g_fonts, &state);
            dglabFramebufferEnd();

            drawn_selected = state.selected;
            drawn_liveness = state.sysmodule_ok;
            drawn_generation = g_display_generation;
            have_drawn = true;
        }
    }

    return DglabMenuResult_Exit;
}

// ---------------------------------------------------------------------------
// The motion mode
// ---------------------------------------------------------------------------

// The defaults plus whatever the advanced screen last saved. Any missing or
// unreadable file simply leaves the defaults in place.
static void motionSettingsLoad(DglabMotionFeedConfig* config)
{
    char text[512];
    size_t size;
    FILE* file;

    dglabMotionSettingsDefault(config);

    file = fopen(MOTION_CONFIG_PATH, "r");

    if (file == NULL)
        return;

    size = fread(text, 1, sizeof(text) - 1, file);
    text[size] = '\0';
    fclose(file);

    dglabMotionSettingsParse(config, text);
}

// Written on every change: a few hundred bytes, and losing a tuning session to a
// crash or a power off would be worse.
static bool motionSettingsSave(const DglabMotionFeedConfig* config)
{
    char text[512];
    FILE* file;

    dglabMotionSettingsSerialize(config, text, sizeof(text));

    file = fopen(MOTION_CONFIG_PATH, "w");

    if (file == NULL)
        return false;

    fputs(text, file);
    fclose(file);

    return true;
}

// The live values are throttled to about 5Hz: at 60Hz every frame would change
// the screen and undo the socket screen's redraw-only-on-change policy. Sampling
// and uploading still happen every frame.
#define MOTION_DISPLAY_FRAMES 12
#define MOTION_DRAIN_MAX 17u

#define MOTION_CHANNEL_A 1u
#define MOTION_CHANNEL_B 2u

static bool slotsHaveStrength(const DglabNetWaveformSlot* slots, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (slots[i].strength > 0)
            return true;
    }

    return false;
}

static Result uploadSlots(Service* dglab, u32 channel, const DglabNetWaveformSlot* slots,
    size_t count)
{
    DglabNetWaveformRequest request;

    if (count == 0 || count > DGLAB_NET_WAVEFORM_MAX_SLOTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    memset(&request, 0, sizeof(request));
    request.channel = channel;
    request.mode = DglabNetWaveform_Append;
    request.slot_count = (u32)count;
    memcpy(request.slots, slots, count * sizeof(slots[0]));

    return serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_WAVEFORM, request);
}

static void runMotionView(Service* dglab, PadState* pad)
{
    DglabMotionFeedConfig config;
    DglabMotionFeed feed_a;
    DglabMotionFeed feed_b;
    DglabMotionScreenState state;
    DglabNetWaveformSlot slots[DGLAB_NET_WAVEFORM_MAX_SLOTS];
    u64 last_ticks;
    u32 frame = 0;
    u32 drawn_generation = 0;
    bool described = false;
    // The connection the row is showing, so only a change writes a log line. The
    // first poll fills them in without logging: the describe line above already
    // carries the state the mode started in.
    bool link_known = false;
    bool left_connected = false;
    bool right_connected = false;
    // The pad's own report for this frame, kept for the log line below.
    u32 pad_device = 0;
    u32 pad_styles = 0;
    u32 pad_attributes = 0;
    bool pad_handheld = false;

    motionSettingsLoad(&config);
    dglabMotionFeedInit(&feed_a, &config);
    dglabMotionFeedInit(&feed_b, &config);

    memset(&state, 0, sizeof(state));
    state.link = dglabString(DglabString_StateNotStarted);
    state.last_upload = "";

    // Whatever the test buttons left queued should not play underneath the
    // motion stream.
    noteCommand(dglabString(DglabString_CmdClear), sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);

    dglabJoyconStart();

    last_ticks = armGetSystemTick();

    while (appletMainLoop()) {
        DglabMotionSample samples[MOTION_DRAIN_MAX];
        u32 elapsed_ns;
        u64 now;
        u64 down;

        now = armGetSystemTick();
        elapsed_ns = (u32)armTicksToNs(now - last_ticks);
        last_ticks = now;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        // The server state is asked for every frame, not only when the panel is
        // rebuilt: A starts and stops from what this frame saw, and the bottom
        // bar's label comes from the same pass - the key and the label cannot
        // disagree. The panel still refreshes at its own rate below.
        {
            DglabNetStatus status;
            bool status_ok = R_SUCCEEDED(
                serviceDispatchOut(dglab, DGLAB_IPC_CMD_NET_STATUS, status));

            state.server_running = status_ok &&
                (status.state == DglabNetState_Listening ||
                    status.state == DglabNetState_Paired);

            // The motion page is the other place the server can be started, so
            // it has to keep the console's sleep timer in step with it too.
            appAutoSleepFollow(state.server_running);

            if (!status_ok) {
                state.link = dglabString(DglabString_StateIpcFailed);
                state.link_tone = DglabCmdTone_Error;
            } else {
                state.link = dglabNetStateText(status.state);
                state.link_tone = (status.state == DglabNetState_Paired) ? DglabCmdTone_Ok
                                                                        : DglabCmdTone_Warn;
            }
        }

        if (down & HidNpadButton_B)
            break;

        // The same shortcuts as the socket page: A starts and stops the server,
        // X clears what a test left playing, ZL and ZR fire one channel each,
        // and the D-pad dials the two channel strengths (up/down A, left/right
        // B). Nothing is focused here, so the D-pad has no other job.
        if (down & HidNpadButton_A)
            toggleServer(dglab, state.server_running);

        // Y takes a fresh set of sensor handles. The mode's handles describe the
        // assignment the console had when they were taken, so a Joy-Con that was
        // turned off or plugged back in keeps its row on "not connected" until
        // they are taken again - this is that, without leaving the page.
        if (down & HidNpadButton_Y) {
            dglabJoyconRescan();
            logPushLine("motion rescan (Y)");
        }

        testChannelButtons(dglab, down);
        adjustStrengthFromDirections(dglab, down);
        repeatStrengthFromDirections(dglab, padGetButtons(pad),
            armTicksToNs(armGetSystemTick()));

        // Which sides this mode can read at all, from the pad the buttons come
        // from: the six-axis handles keep handing over readings for a Joy-Con
        // that is attached to the console or switched off, so they are not a
        // connection signal (hardware report, 2026-09-17 - and the reason the two
        // rows never went back to 未连接). `padIsHandheld` covers "plugged back
        // in", the two attribute bits cover "switched off / not there".
        {
            // The per-side answer, when the console gives one: JoyLeft/JoyRight
            // mean the controller is detached and usable as this mode's input,
            // HandheldLeft/Right mean it is clipped onto the console (moving it
            // moves the console, which is not this mode), and a bit that is clear
            // means that side is switched off or gone.
            u32 device = hidGetNpadDeviceType(HidNpadIdType_No1);
            u32 styles = padGetStyleSet(pad);
            u32 attributes = padGetAttributes(pad);
            bool handheld = padIsHandheld(pad);
            bool left_ok;
            bool right_ok;

            if (device != 0) {
                left_ok = (device & HidDeviceTypeBits_JoyLeft) != 0;
                right_ok = (device & HidDeviceTypeBits_JoyRight) != 0;
            } else if (styles != 0 || (attributes & HidNpadAttribute_IsConnected) != 0) {
                // No device type: the pad state the buttons come from still says
                // whether the console is handheld and which halves are there.
                left_ok = !handheld &&
                    (styles & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadJoyLeft)) != 0 &&
                    (attributes & HidNpadAttribute_IsLeftConnected) != 0;
                right_ok = !handheld &&
                    (styles & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadJoyRight)) != 0 &&
                    (attributes & HidNpadAttribute_IsRightConnected) != 0;
            } else {
                // Nothing is answering, not even the pad: keep the old behaviour,
                // which judges each side by its readings alone.
                left_ok = true;
                right_ok = true;
            }

            pad_device = device;
            pad_styles = styles;
            pad_attributes = attributes;
            pad_handheld = handheld;

            // Drain both sides every frame: the sensors run faster than this
            // loop, and a reading that is not collected now is gone.
            for (size_t i = 0, count = dglabJoyconPoll(DglabJoycon_Left, samples,
                     MOTION_DRAIN_MAX, left_ok); i < count; i++)
                dglabMotionFeedAddSample(&feed_a, &samples[i]);

            for (size_t i = 0, count = dglabJoyconPoll(DglabJoycon_Right, samples,
                     MOTION_DRAIN_MAX, right_ok); i < count; i++)
                dglabMotionFeedAddSample(&feed_b, &samples[i]);

        }

        // One line about the sensor handles, once per visit (the log page shows
        // it, and the whole line goes to the file on the SD card). Which handles
        // a console hands over, and which of them answer, is a hardware fact:
        // this is what makes "the row says not connected while the waveform
        // plays" answerable from a log instead of a guess.
        if (!described) {
            char left[160];
            char right[160];
            char line[448];

            described = true;
            dglabJoyconDescribe(DglabJoycon_Left, left, sizeof(left));
            dglabJoyconDescribe(DglabJoycon_Right, right, sizeof(right));
            snprintf(line, sizeof(line),
                "motion device 0x%08X styles 0x%08X attrs 0x%08X handheld %u | %s | %s",
                (unsigned)pad_device, (unsigned)pad_styles, (unsigned)pad_attributes,
                pad_handheld ? 1u : 0u, left, right);
            logPushLine(line);
        }

        // A side going away or coming back is worth a line of its own: whether a
        // sleeping or removed Joy-Con really clears IsConnected is one of the
        // things only hardware can answer (docs/joycon-input.md), and it is the
        // state the row is about. Only changes are logged, so a still session
        // writes nothing.
        {
            bool left_now = dglabJoyconIsConnected(DglabJoycon_Left);
            bool right_now = dglabJoyconIsConnected(DglabJoycon_Right);

            if (link_known) {
                // The line carries the side's own description, so a change in the
                // log says *why* it happened: which handle answered, how long the
                // side had been quiet, whether it had just re-acquired a set.
                if (left_now != left_connected) {
                    char detail[160];
                    char line[224];

                    dglabJoyconDescribe(DglabJoycon_Left, detail, sizeof(detail));
                    snprintf(line, sizeof(line), "motion left %s: %s",
                        left_now ? "connected" : "disconnected", detail);
                    logPushLine(line);
                }

                if (right_now != right_connected) {
                    char detail[160];
                    char line[224];

                    dglabJoyconDescribe(DglabJoycon_Right, detail, sizeof(detail));
                    snprintf(line, sizeof(line), "motion right %s: %s",
                        right_now ? "connected" : "disconnected", detail);
                    logPushLine(line);
                }
            }

            left_connected = left_now;
            right_connected = right_now;
            link_known = true;
        }

        // A batch that is all silence is dropped instead of uploaded: that is
        // what makes a still controller cost no traffic at all.
        {
            size_t produced = dglabMotionFeedAdvance(&feed_a, elapsed_ns, slots,
                DGLAB_NET_WAVEFORM_MAX_SLOTS);

            if (produced > 0 && slotsHaveStrength(slots, produced))
                noteCommand(dglabString(DglabString_CmdWaveformA),
                    uploadSlots(dglab, MOTION_CHANNEL_A, slots, produced), NULL);
        }

        {
            size_t produced = dglabMotionFeedAdvance(&feed_b, elapsed_ns, slots,
                DGLAB_NET_WAVEFORM_MAX_SLOTS);

            if (produced > 0 && slotsHaveStrength(slots, produced))
                noteCommand(dglabString(DglabString_CmdWaveformB),
                    uploadSlots(dglab, MOTION_CHANNEL_B, slots, produced), NULL);
        }

        // The header's status is asked for every frame (the shared poll is what
        // paces the PING); the live values below refresh a few times a second,
        // and a rebuilt display is drawn at once, whatever that counter says.
        state.sysmodule_ok = appSysmoduleOk(dglab);

        if (++frame % MOTION_DISPLAY_FRAMES == 1 || drawn_generation != g_display_generation) {
            DglabCanvas canvas;

            state.left_connected = dglabJoyconIsConnected(DglabJoycon_Left);
            state.right_connected = dglabJoyconIsConnected(DglabJoycon_Right);
            state.moving_a = dglabMotionFeedIsMoving(&feed_a);
            state.moving_b = dglabMotionFeedIsMoving(&feed_b);
            state.level_a = (unsigned)(dglabMotionFeedLevel(&feed_a) * 100.0f + 0.5f);
            state.level_b = (unsigned)(dglabMotionFeedLevel(&feed_b) * 100.0f + 0.5f);
            state.frequency_a = dglabMotionFeedFrequencyMs(&feed_a);
            state.frequency_b = dglabMotionFeedFrequencyMs(&feed_b);
            state.channel_strength_a = g_test_strength_a;
            state.channel_strength_b = g_test_strength_b;
            state.last_upload = g_last_command;
            state.last_upload_tone = g_last_command_tone;

            if (dglabFramebufferBegin(&canvas)) {
                dglabMotionScreenDraw(&canvas, &g_fonts, &state);
                dglabFramebufferEnd();

                drawn_generation = g_display_generation;
            }
        }
    }

    // Stop the stream before leaving: the sensors go quiet and the queued
    // waveform is cleared, so nothing keeps playing from the menu.
    dglabJoyconStop();
    noteCommand(dglabString(DglabString_CmdClear), sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);
}

// ---------------------------------------------------------------------------
// The touch mode
// ---------------------------------------------------------------------------

// The mode itself. Everything it decides about a position - which half, which
// finger, and what value and density that means - lives in
// dglab/nro/touch_feed.h and is held down by tests/touch; what is left here is
// the frame loop, and the same 25ms slot producer the motion mode drives.
#define TOUCH_DISPLAY_FRAMES 2
#define TOUCH_LOG_MAX 200

// The DG-LAB channels the two halves drive, and the strings the "last cmd" line
// names them with. The halves are A and B, the channels are 1 and 2 (dglab/ipc.h);
// the two numberings stay apart on purpose (docs/touch-input.md).
#define TOUCH_CHANNEL_A 1u
#define TOUCH_CHANNEL_B 2u

static u32 touchChannelNumber(unsigned channel)
{
    return channel == (unsigned)DglabTouchChannelValue_A ? TOUCH_CHANNEL_A : TOUCH_CHANNEL_B;
}

static DglabString touchChannelCommand(unsigned channel)
{
    return channel == (unsigned)DglabTouchChannelValue_A ? DglabString_CmdWaveformA
                                                         : DglabString_CmdWaveformB;
}

// One log line: what the last poll saw, plus what the mode made of it. Written
// when the mode starts and whenever the halves that have a finger change, so a
// whole probe run is one readable stretch of the SD card's log.
static void touchLogLine(const char* tag)
{
    char line[TOUCH_LOG_MAX];
    size_t length = dglabTouchDescribe(line, sizeof(line));

    if (length == 0)
        return;

    if (length + 16 < sizeof(line))
        snprintf(line + length, sizeof(line) - length, " | %s", tag);

    logPushLine(line);
}

static void runTouchView(Service* dglab, PadState* pad)
{
    DglabMotionFeedConfig config;
    DglabTouchFeed touch_feed;
    DglabMotionFeed stream[DglabTouchChannelValue_Count];
    DglabTouchScreenState state;
    DglabNetWaveformSlot slots[DGLAB_NET_WAVEFORM_MAX_SLOTS];
    u64 last_ticks;
    u32 frame = 0;
    u32 drawn_generation = 0;
    bool described = false;
    bool have_last = false;
    bool last_held_a = false;
    bool last_held_b = false;

    // Entering the mode is what initializes the panel: nothing else in this NRO
    // touches it (nro/AGENTS.md). libnx has no error to report here - a refusal
    // is its fatal error page - which is why the log's first line is the answer
    // the probe run exists for.
    dglabTouchStart();

    // One parameter file for both modes (docs/touch-input.md): the envelope, the
    // idle stop, the two frequency ends and the strength ceiling are the numbers
    // the Advanced page edits for the motion mode. The one thing this mode decides
    // for itself is that its density is the panel's horizontal axis and not its
    // level.
    motionSettingsLoad(&config);
    config.frequency_follows_level = false;

    for (unsigned channel = 0; channel < (unsigned)DglabTouchChannelValue_Count; channel++)
        dglabMotionFeedInit(&stream[channel], &config);

    dglabTouchFeedInit(&touch_feed);

    memset(&state, 0, sizeof(state));
    state.link = dglabString(DglabString_StateNotStarted);
    state.last_upload = "";
    // The density switch is a parameter rather than a per-frame value: the page
    // only needs it to know whether there is a horizontal axis worth drawing.
    state.density_fixed = config.density_fixed;

    // Whatever the test buttons left queued should not play underneath the mode.
    noteCommand(dglabString(DglabString_CmdClear), sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);

    last_ticks = armGetSystemTick();

    while (appletMainLoop()) {
        DglabTouchFrame readings;
        u32 elapsed_ns;
        u64 now;
        u64 down;

        // The slots stay paced by time rather than by frames, like the motion
        // mode's: a slow frame produces several of them at once.
        now = armGetSystemTick();
        elapsed_ns = (u32)armTicksToNs(now - last_ticks);
        last_ticks = now;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        // The same server poll and shortcuts as the socket and motion pages: A
        // starts and stops the server, X clears, ZL and ZR fire one channel, and
        // the D-pad dials the two channel strengths.
        {
            DglabNetStatus status;
            bool status_ok = R_SUCCEEDED(
                serviceDispatchOut(dglab, DGLAB_IPC_CMD_NET_STATUS, status));

            state.server_running = status_ok &&
                (status.state == DglabNetState_Listening ||
                    status.state == DglabNetState_Paired);

            appAutoSleepFollow(state.server_running);

            if (!status_ok) {
                state.link = dglabString(DglabString_StateIpcFailed);
                state.link_tone = DglabCmdTone_Error;
            } else {
                state.link = dglabNetStateText(status.state);
                state.link_tone = (status.state == DglabNetState_Paired) ? DglabCmdTone_Ok
                                                                        : DglabCmdTone_Warn;
            }
        }

        if (down & HidNpadButton_B)
            break;

        if (down & HidNpadButton_A)
            toggleServer(dglab, state.server_running);

        testChannelButtons(dglab, down);
        adjustStrengthFromDirections(dglab, down);
        repeatStrengthFromDirections(dglab, padGetButtons(pad),
            armTicksToNs(armGetSystemTick()));

        // The panel, then the mapping, then the slots: the same three steps the
        // motion mode takes with a sensor sample, which is what lets the two modes
        // share the envelope, the idle stop and the parameters.
        dglabTouchPoll(&readings);
        dglabTouchFeedUpdate(&touch_feed, &readings);

        for (unsigned channel = 0; channel < (unsigned)DglabTouchChannelValue_Count; channel++) {
            const DglabTouchChannelState* target = dglabTouchFeedChannel(&touch_feed, channel);

            if (target->held)
                dglabMotionFeedSetTarget(&stream[channel], target->level, target->density);
        }

        // An all-zero batch is not uploaded, which is the rule the motion mode
        // already works by: a finger held on the bottom rule, and everything after
        // a release has decayed, costs no traffic at all.
        for (unsigned channel = 0; channel < (unsigned)DglabTouchChannelValue_Count; channel++) {
            size_t produced = dglabMotionFeedAdvance(&stream[channel], elapsed_ns, slots,
                DGLAB_NET_WAVEFORM_MAX_SLOTS);

            if (produced > 0 && slotsHaveStrength(slots, produced))
                noteCommand(dglabString(touchChannelCommand(channel)),
                    uploadSlots(dglab, touchChannelNumber(channel), slots, produced), NULL);
        }

        {
            const DglabTouchChannelState* a =
                dglabTouchFeedChannel(&touch_feed, DglabTouchChannelValue_A);
            const DglabTouchChannelState* b =
                dglabTouchFeedChannel(&touch_feed, DglabTouchChannelValue_B);

            state.held_a = a->held;
            state.x_a = a->x;
            state.y_a = a->y;
            state.level_a = (unsigned)(dglabMotionFeedLevel(&stream[DglabTouchChannelValue_A]) *
                                       100.0f + 0.5f);
            state.frequency_a = dglabMotionFeedFrequencyMs(&stream[DglabTouchChannelValue_A]);

            state.held_b = b->held;
            state.x_b = b->x;
            state.y_b = b->y;
            state.level_b = (unsigned)(dglabMotionFeedLevel(&stream[DglabTouchChannelValue_B]) *
                                       100.0f + 0.5f);
            state.frequency_b = dglabMotionFeedFrequencyMs(&stream[DglabTouchChannelValue_B]);
        }

        state.docked = !dglabTouchHandheld();

        // One line at the start and one per change of which halves have a finger:
        // the log then holds the whole session - the LIFO depth the panel answered
        // with, the coordinates, and what the mode made of them.
        if (!described) {
            described = true;
            touchLogLine("start");
        } else if (!have_last || state.held_a != last_held_a || state.held_b != last_held_b) {
            char tag[80];

            if (state.held_a && state.held_b) {
                snprintf(tag, sizeof(tag), "both A %u/%ums B %u/%ums", state.level_a,
                    state.frequency_a, state.level_b, state.frequency_b);
            } else if (state.held_a) {
                snprintf(tag, sizeof(tag), "A %u/%ums", state.level_a, state.frequency_a);
            } else if (state.held_b) {
                snprintf(tag, sizeof(tag), "B %u/%ums", state.level_b, state.frequency_b);
            } else {
                snprintf(tag, sizeof(tag), "none");
            }

            touchLogLine(tag);
        }

        last_held_a = state.held_a;
        last_held_b = state.held_b;
        have_last = true;

        state.sysmodule_ok = appSysmoduleOk(dglab);

        if (frame % TOUCH_DISPLAY_FRAMES == 0 || drawn_generation != g_display_generation) {
            DglabCanvas canvas;

            state.channel_strength_a = g_test_strength_a;
            state.channel_strength_b = g_test_strength_b;
            state.last_upload = g_last_command;
            state.last_upload_tone = g_last_command_tone;

            if (dglabFramebufferBegin(&canvas)) {
                dglabTouchScreenDraw(&canvas, &g_fonts, &state);
                dglabFramebufferEnd();

                drawn_generation = g_display_generation;
            }
        }

        frame++;
    }

    // Leaving the mode clears what the mode and the test keys may have left
    // playing, the same way the motion mode does.
    noteCommand(dglabString(DglabString_CmdClear), sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);
}

// Error path: the console is used instead of the framebuffer, because it is the
// rendering path that is known to work on real hardware. Without the sysmodule
// there is nothing to draw anyway, and an empty window tells the user nothing.

// ---------------------------------------------------------------------------
// The Bluetooth page
// ---------------------------------------------------------------------------

#define BLE_ADDRESS_PATH CONFIG_DIR "/dglab-ble-address.txt"

// The address the sysmodule discovered and wrote out (docs/ble-poc.md,
// "设备地址：自动发现"). Nothing to connect to without it, and the page says so
// rather than guessing.
static bool loadBleAddress(u8 out[6])
{
    FILE* file = fopen(BLE_ADDRESS_PATH, "r");
    char line[64];
    unsigned int bytes[6];

    if (file == NULL)
        return false;

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return false;
    }

    fclose(file);

    if (sscanf(line, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3],
            &bytes[4], &bytes[5]) != 6)
        return false;

    for (int i = 0; i < 6; i++)
        out[i] = (u8)bytes[i];

    return true;
}

static bool bleStateIsActive(u32 state)
{
    return state == DglabBleState_Connecting || state == DglabBleState_Connected;
}

// The Bluetooth page. Starting is two steps because the transport needs two:
// the driver-level probe brings the stack up (it is the only thing that may call
// InitializeBle/EnableBle, docs/history.md §28), and the session then connects
// and streams. Leaving the page stops the session: one left running would keep
// driving the device with nobody watching.
static void runBleView(Service* dglab, PadState* pad)
{
    DglabBlePageState state;
    DglabBlePageState drawn;
    bool have_drawn = false;
    u32 drawn_generation = 0;
    int offset = 0;
    int drawn_offset = 0;

    memset(&state, 0, sizeof(state));
    memset(&drawn, 0, sizeof(drawn));
    state.soft_limit = 20u;

    while (appletMainLoop()) {
        DglabCanvas canvas;
        DglabBleStatus status;
        DglabPocStatus poc_status;
        Result status_rc;
        u64 down;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        if (down & HidNpadButton_B) {
            serviceDispatch(dglab, DGLAB_IPC_CMD_BLE_STOP);
            return;
        }

        // Up and down set the ceiling the device enforces; the row shows it, so
        // there is no hint for it in the bottom bar.
        if (down & HidNpadButton_Up)
            state.soft_limit = (state.soft_limit + 5u > 200u) ? 200u : state.soft_limit + 5u;

        if (down & HidNpadButton_Down)
            state.soft_limit = (state.soft_limit >= 5u) ? state.soft_limit - 5u : 0u;

        if (down & HidNpadButton_X)
            serviceDispatch(dglab, DGLAB_IPC_CMD_BLE_STOP);

        if ((down & HidNpadButton_A) && !state.starting && !bleStateIsActive(state.status.state)) {
            u8 address[6];

            if (loadBleAddress(address)) {
                DglabPocStartRequest request = { 0 };
                DglabPocActionRequest action = { .action = DglabPocAction_ProbeBtdrvScan };

                request.flags = DGLAB_POC_START_FLAG_TARGET_ADDRESS;
                memcpy(request.target_address, address, sizeof(request.target_address));
                memcpy(state.status.address, address, sizeof(state.status.address));

                if (R_SUCCEEDED(serviceDispatchIn(dglab, DGLAB_IPC_POC_CMD_START, request)) &&
                    R_SUCCEEDED(serviceDispatchIn(dglab, DGLAB_IPC_POC_CMD_ACTION, action)))
                    state.starting = true;
            }
        }

        memset(&status, 0, sizeof(status));
        status_rc = serviceDispatchOut(dglab, DGLAB_IPC_CMD_BLE_STATUS, status);
        state.sysmodule_ok = R_SUCCEEDED(status_rc);

        if (R_SUCCEEDED(status_rc)) {
            u8 address[6];

            memcpy(address, state.status.address, sizeof(address));
            state.status = status;
            memcpy(state.status.address, address, sizeof(address));
        }

        // Step 1 of the start sequence: wait for the driver-level session to
        // finish, then send step 2. The PoC status is the only place that says
        // whether a session is still running.
        if (state.starting) {
            u8 address[6];

            memset(&poc_status, 0, sizeof(poc_status));
            state.driver_running = R_SUCCEEDED(
                serviceDispatchOut(dglab, DGLAB_IPC_POC_CMD_STATUS, poc_status)) &&
                poc_status.state == DglabPocState_Initializing;

            if (!state.driver_running && loadBleAddress(address)) {
                DglabBleStartRequest request = { 0 };

                state.starting = false;
                request.soft_limit = state.soft_limit;
                memcpy(request.address, address, sizeof(request.address));
                memcpy(state.status.address, address, sizeof(state.status.address));
                serviceDispatchIn(dglab, DGLAB_IPC_CMD_BLE_START, request);
            }
        }

        state.offset = offset;
        offset = dglabListScrollClamp(offset,
            dglabBleContentHeight(&g_fonts, &state) -
                (DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP));
        state.offset = offset;

        if (have_drawn && memcmp(&drawn, &state, sizeof(state)) == 0 &&
            drawn_offset == offset && drawn_generation == g_display_generation)
            continue;

        if (dglabFramebufferBegin(&canvas)) {
            dglabBleDraw(&canvas, &g_fonts, &state);
            dglabFramebufferEnd();

            drawn = state;
            drawn_offset = offset;
            drawn_generation = g_display_generation;
            have_drawn = true;
        }
    }
}

// ---------------------------------------------------------------------------
// The about screen
// ---------------------------------------------------------------------------

static void runAboutView(Service* dglab, PadState* pad)
{
    DglabIpcVersion version = { 0 };
    DglabAboutState state;
    // The page's own text decides how far it scrolls, so the offset is kept here
    // and clamped against a fresh measurement every frame: switching the language
    // changes the height, and the offset has to follow it.
    int offset = 0;
    int drawn_offset = 0;
    // The language is a page input like any other: left and right change both the
    // strings and the row heights, and leaving it out of the redraw test is how a
    // language switch used to go unseen until the next scroll key pressed.
    DglabLanguage drawn_preference = DglabLanguage_Count;
    DglabLanguage drawn_resolved = DglabLanguage_Count;
    // The theme is a page input as well: Y changes both the rows and the palette
    // behind them, so both have to be in the redraw test - the language row
    // taught this page that a key whose effect is left out of it looks broken.
    DglabThemeMode drawn_theme = DglabThemeMode_Count;
    bool drawn_liveness = false;
    bool have_drawn = false;
    u32 drawn_generation = 0;

    serviceDispatchOut(dglab, DGLAB_IPC_CMD_GET_VERSION, version);

    while (appletMainLoop()) {
        DglabCanvas canvas;
        u64 down;
        int step = 0;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        if (down & HidNpadButton_B)
            return;

        // Left and right switch the language and up and down scroll, which is
        // what the bottom bar says - the scroll hint only while the page really
        // is taller than the screen. There is nothing to focus on this page.
        if ((down & HidNpadButton_Left) || (down & HidNpadButton_Right)) {
            // Left and right both cycle: with three values there is no natural
            // direction, and the row shows what it became.
            g_settings.language = dglabLanguageNext(g_settings.language);
            appSettingsSave();
            appLanguageApply();
        }

        // Y cycles the colour theme: follow the console, light, dark and back.
        // The palette is applied at once - the page redraws with it, and every
        // other page draws with it when it is opened next.
        if (down & HidNpadButton_Y) {
            g_settings.theme = dglabThemeModeNext(g_settings.theme);
            appSettingsSave();
            appThemeApply();
        }

        if (down & HidNpadButton_Up)
            step -= DGLAB_ABOUT_SCROLL_STEP;

        if (down & HidNpadButton_Down)
            step += DGLAB_ABOUT_SCROLL_STEP;

        memset(&state, 0, sizeof(state));
        state.preference = g_settings.language;
        state.resolved = g_language;
        state.theme = g_settings.theme;
        state.theme_system_is_dark = g_system_is_dark;
        state.app_version = DGLAB_APP_VERSION;
        state.build_id = DGLAB_BUILD_STAMP;
        state.ipc_version = version;
        state.github_url = "https://github.com/livcm/DGLAB-NX";
        state.sysmodule_ok = appSysmoduleOk(dglab);

        offset += step;
        offset = dglabListScrollClamp(offset,
            dglabAboutContentHeight(&g_fonts, &state) -
                (DGLAB_PAGE_CONTENT_BOTTOM - DGLAB_PAGE_CONTENT_TOP));
        state.offset = offset;

        if (have_drawn && offset == drawn_offset && state.sysmodule_ok == drawn_liveness &&
            state.preference == drawn_preference && state.resolved == drawn_resolved &&
            state.theme == drawn_theme && drawn_generation == g_display_generation)
            continue;

        if (dglabFramebufferBegin(&canvas)) {
            dglabAboutDraw(&canvas, &g_fonts, &state);
            dglabFramebufferEnd();

            drawn_offset = offset;
            drawn_preference = state.preference;
            drawn_resolved = state.resolved;
            drawn_theme = state.theme;
            drawn_liveness = state.sysmodule_ok;
            drawn_generation = g_display_generation;
            have_drawn = true;
        }
    }
}

// ---------------------------------------------------------------------------
// The advanced screen
// ---------------------------------------------------------------------------

// One press is worth exactly one step. A repeat only starts after a deliberate
// hold, and slower than the strength keys: a parameter that jumped by accident
// cannot be undone by tapping the other way once.
#define ADVANCED_HOLD_NS (500ull * 1000000ull)
#define ADVANCED_REPEAT_NS (200ull * 1000000ull)

static int horizontalDirection(u64 buttons)
{
    int direction = 0;

    if (buttons & HidNpadButton_Left)
        direction -= 1;

    if (buttons & HidNpadButton_Right)
        direction += 1;

    return direction;
}

static void runAdvancedView(Service* dglab, PadState* pad)
{
    DglabMotionFeedConfig config;
    DglabAdvancedState state;
    u64 hold_started_ns = 0;
    u64 last_repeat_ns = 0;
    unsigned drawn_selected = ~0u;
    u32 revision = 0;
    u32 drawn_revision = ~0u;
    bool drawn_saved = false;
    bool drawn_liveness = false;
    u32 drawn_generation = ~0u;

    motionSettingsLoad(&config);

    memset(&state, 0, sizeof(state));
    state.config = &config;
    state.saved = true;
    state.sysmodule_ok = appSysmoduleOk(dglab);

    while (appletMainLoop()) {
        DglabCanvas canvas;
        u64 now_ns = armTicksToNs(armGetSystemTick());
        u64 down;
        u64 held;
        int press;
        int hold_direction;

        padUpdate(pad);
        down = padGetButtonsDown(pad);
        held = padGetButtons(pad);
        state.sysmodule_ok = appSysmoduleOk(dglab);

        if (down & HidNpadButton_B) {
            motionSettingsSave(&config);
            return;
        }

        if (down & HidNpadButton_Up && state.selected > 0) {
            state.selected--;
            hold_started_ns = 0; // a held direction must not keep editing the new row
        }

        if (down & HidNpadButton_Down && state.selected + 1 < (unsigned)DglabMotionSetting_Count) {
            state.selected++;
            hold_started_ns = 0;
        }

        if (down & HidNpadButton_Y) {
            dglabMotionSettingsDefault(&config);
            state.saved = motionSettingsSave(&config);
            revision++;
            hold_started_ns = 0;
        }

        press = horizontalDirection(down);
        hold_direction = horizontalDirection(held);

        if (press != 0) {
            dglabMotionSettingsStep(&config, state.selected, press);
            state.saved = motionSettingsSave(&config);
            revision++;
            hold_started_ns = now_ns ? now_ns : 1u;
            last_repeat_ns = 0;
        } else if (hold_direction == 0) {
            hold_started_ns = 0;
        } else if (hold_started_ns == 0) {
            hold_started_ns = now_ns ? now_ns : 1u;
        } else if (now_ns - hold_started_ns >= ADVANCED_HOLD_NS &&
                   (last_repeat_ns == 0 || now_ns - last_repeat_ns >= ADVANCED_REPEAT_NS)) {
            dglabMotionSettingsStep(&config, state.selected, hold_direction);
            state.saved = motionSettingsSave(&config);
            revision++;
            last_repeat_ns = now_ns;
        }

        if (state.selected == drawn_selected && revision == drawn_revision &&
            state.saved == drawn_saved && drawn_liveness == state.sysmodule_ok &&
            drawn_generation == g_display_generation)
            continue;

        if (dglabFramebufferBegin(&canvas)) {
            dglabAdvancedDraw(&canvas, &g_fonts, &state);
            dglabFramebufferEnd();

            drawn_selected = state.selected;
            drawn_revision = revision;
            drawn_saved = state.saved;
            drawn_liveness = state.sysmodule_ok;
            drawn_generation = g_display_generation;
        }
    }
}
static void showNotice(PadState* pad, const char* headline, ...)
{
    va_list args;

    consoleInit(NULL);

    printf("DGLAB-NX\n\n");

    va_start(args, headline);
    vprintf(headline, args);
    va_end(args);

    printf("\n\n");
    printf("The socket server lives in the sysmodule, so the front end has nothing\n");
    printf("to show without it. Install it and reboot the console:\n\n");
    printf("    build/00FF072107210721/  ->  SD:/atmosphere/contents/00FF072107210721/\n");
    printf("\nThen start this homebrew again.\n");
    printf("\nPress + to exit.\n");
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(pad);

        if (padGetButtonsDown(pad) & HidNpadButton_Plus)
            break;

        consoleUpdate(NULL);
    }

    consoleExit(NULL);
}

// The language files are checked before the first frame is drawn, so this is
// the one screen that cannot be translated: it is plain ASCII on libnx's
// console, which has no room for the CJK the UI itself uses (docs/nro-ui.md).
static void showStartupNotice(PadState* pad, const char* text)
{
    consoleInit(NULL);

    printf("DGLAB-NX\n\n");
    printf("%s", text);
    printf("\nPress + to exit.\n");
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(pad);

        if (padGetButtonsDown(pad) & HidNpadButton_Plus)
            break;

        consoleUpdate(NULL);
    }

    consoleExit(NULL);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    // The UI text lives in lang/<code>.json on the SD card, so the files are
    // read before anything else happens: without one there is nothing to draw
    // (docs/nro-ui.md). One file is enough - a language without a file of its
    // own follows the one that loaded.
    DglabLangReport lang;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    dglabLangFilesLoad(DGLAB_LANG_DIR, &lang);

    if (!lang.loaded) {
        showStartupNotice(&pad, lang.error);
        return 0;
    }

    Service dglab;
    Result service_result = smGetService(&dglab, DGLAB_IPC_SERVICE_NAME);

    if (R_FAILED(service_result)) {
        showNotice(&pad, "DGLAB-NX: the sysmodule is not running (0x%08X)", service_result);
        return 0;
    }

    if (!dglabFramebufferOpen()) {
        showNotice(&pad, "DGLAB-NX: the framebuffer could not be created");
        serviceClose(&dglab);
        return 0;
    }

    logFileOpen();

    // A file that loaded but is missing entries is not fatal: the entries fall
    // back to the other language and the reason is reported in the log panel
    // of the socket screen instead.
    for (unsigned i = 0; i < lang.notes; i++)
        logPushLine(lang.note[i]);

    // set:sys is what tells us whether the console runs its light or its dark
    // theme. A console that does not offer it (or an applet that may not ask)
    // is not a reason to stop: "follow the console" then means dark, and the
    // log panel carries the reason the row says so.
    g_set_sys_ready = R_SUCCEEDED(setsysInitialize());

    if (!g_set_sys_ready)
        logPushLine("theme: set:sys unavailable, so following the console means dark");

    appSettingsLoad();
    appLanguageApply();
    appThemeApply();

    if (g_set_sys_ready && !g_system_theme_ok)
        logPushLine("theme: the console did not report a colour set, following it means dark");

    // Docking and undocking changes the frame the NRO draws into (720p handheld,
    // 1080p docked), so the display is built again when the console says it
    // moved. The hook only records that; the rebuild happens between frames.
    AppletHookCookie display_hook;

    appletHook(&display_hook, appletHookCallback, NULL);

    while (true) {
        if (g_display_mode_dirty) {
            g_display_mode_dirty = false;
            appDisplaySuspend();

            if (!appDisplayReopen()) {
                showNotice(&pad, "DGLAB-NX: the framebuffer could not be created");
                break;
            }
        }

        DglabMenuResult selection = runMenuView(&dglab, &pad);

        if (selection == DglabMenuResult_Exit)
            break;

        if (selection == DglabMenuResult_Socket) {
            runSocketView(&dglab, &pad);
            continue;
        }

        if (selection == DglabMenuResult_Ble) {
            runBleView(&dglab, &pad);
            continue;
        }

        if (selection == DglabMenuResult_Motion) {
            runMotionView(&dglab, &pad);
            continue;
        }

        if (selection == DglabMenuResult_Touch) {
            runTouchView(&dglab, &pad);
            continue;
        }

        if (selection == DglabMenuResult_Advanced) {
            runAdvancedView(&dglab, &pad);
            continue;
        }

        if (selection == DglabMenuResult_About) {
            runAboutView(&dglab, &pad);
            continue;
        }

        // The BLE PoC view takes the console and the screen over, so the
        // framebuffer and the shared font are released while it runs and built
        // again afterwards - including the fonts, which used to be left closed
        // (see appDisplayReopen). It drives the IPC session this loop already
        // holds: the sysmodule registers with max_sessions=1, so the view opening
        // its own session would fail with 0x615 while the socket page works.
        appDisplaySuspend();
        dglabBlePocViewRun(&dglab);

        if (!appDisplayReopen())
            break;
    }

    appletUnhook(&display_hook);
    dglabFramebufferClose();

    // Automatic sleep is a console setting the user can change themselves, so
    // leave it the way it was found before the process goes away (and before the
    // log file closes: a failed restore is worth a line).
    appAutoSleepRestore();

    if (g_log_file != NULL)
        fclose(g_log_file);

    if (g_set_sys_ready)
        setsysExit();

    serviceClose(&dglab);

    return 0;
}
