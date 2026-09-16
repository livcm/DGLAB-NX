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
#include <dglab/nro/joycon.h>
#include <dglab/platform/font.h>
#include <dglab/ui/about.h>
#include <dglab/ui/language.h>
#include <dglab/ui/strings.h>
#include <dglab/ui/text.h>
#include <dglab/nro/motion_feed.h>
#include <dglab/nro/motion_settings.h>
#include <dglab/nro/ble_poc_view.h>
#include <dglab/platform/framebuffer.h>
#include <dglab/ui/advanced.h>
#include <dglab/ui/menu.h>
#include <dglab/ui/motion.h>
#include <dglab/ui/screen.h>

#define LOG_POLL_ROUNDS 2

// The log is the only thing here that arrives continuously, and the file it is
// mirrored to does not care about the display refresh rate: the log IPC calls
// are spread over a few frames instead of running 60 times a second.
#define LOG_POLL_INTERVAL_FRAMES 3

// The sysmodule log is mirrored to the SD card, so a test run can be reported
// back as a file instead of as a photo of the screen.
#define DATA_DIR "sdmc:/switch/DGLAB-NX"
#define LOG_FILE_PATH DATA_DIR "/dglab-net.log"
// The motion parameters, so a tuning session does not start over every reboot.
#define MOTION_CONFIG_PATH DATA_DIR "/motion.cfg"
// The app level settings (the UI language).
#define APP_CONFIG_PATH DATA_DIR "/app.cfg"

typedef enum {
    DglabMenuResult_Exit = 0,
    DglabMenuResult_Socket,
    DglabMenuResult_Motion,
    DglabMenuResult_Advanced,
    DglabMenuResult_About,
    DglabMenuResult_BlePoc,
} DglabMenuResult;

// The glyph source every screen draws with: the system shared font at 24px
// (docs/nro-ui.md), or libnx's bitmap font when that cannot be loaded - which
// only has ASCII, so Chinese text would show as gaps.
static DglabGlyphSource* g_text;
static DglabLanguage g_language_pref = DglabLanguage_Auto;
static DglabLanguage g_language = DglabLanguage_English;

static void appLanguageSave(void)
{
    char text[64];
    FILE* file;

    dglabLanguageSerialize(g_language_pref, text, sizeof(text));

    file = fopen(APP_CONFIG_PATH, "w");

    if (file == NULL)
        return;

    fputs(text, file);
    fclose(file);
}

static void appLanguageLoad(void)
{
    char text[128];
    size_t size;
    FILE* file = fopen(APP_CONFIG_PATH, "r");

    g_language_pref = DglabLanguage_Auto;

    if (file == NULL)
        return;

    size = fread(text, 1, sizeof(text) - 1, file);
    text[size] = '\0';
    fclose(file);

    g_language_pref = dglabLanguageParse(text);
}

// Resolves the preference, tells the string tables, and loads the matching face.
static void appLanguageApply(void)
{
    g_language = dglabLanguageResolve(g_language_pref, dglabFontSystemIsChinese());
    dglabStringsSetLanguage(g_language);

    g_text = dglabFontOpen(g_language == DglabLanguage_ChineseSimplified, 24.0f);

    if (g_text == NULL)
        g_text = dglabBitmapGlyphSource(dglabFramebufferFont());
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

// libnx's default init already mounts sdmc, so a failure to open the file is
// reported on screen rather than treated as fatal.
static void logFileOpen(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(DATA_DIR, 0777);

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

// Turns the Result of a command into the single line the screen shows. The
// failures the buttons can actually cause are named; anything else is shown as a
// hex code rather than guessed at. Keep the result under 21 characters: that is
// what the value column of the panel holds.
static void noteCommand(const char* what, Result result, const char* success_note)
{
    const char* outcome = NULL;

    if (R_SUCCEEDED(result)) {
        if (success_note)
            snprintf(g_last_command, sizeof(g_last_command), "%s  ok (%s)", what, success_note);
        else
            snprintf(g_last_command, sizeof(g_last_command), "%s  ok", what);
    } else {
        if (R_DESCRIPTION(result) == LibnxError_NotFound)
            outcome = "no app bound";
        else if (R_DESCRIPTION(result) == LibnxError_BadInput)
            outcome = "rejected";
        else if (R_DESCRIPTION(result) == LibnxError_IoError)
            outcome = "socket error";

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

// The D-pad is the mixer: the vertical axis dials channel A, the horizontal one
// dials channel B. Used for the first press and, at a slower rate, while held.
static void adjustStrengthFromDirections(Service* dglab, u64 buttons)
{
    Result rc;

    if (buttons & HidNpadButton_Up &&
        adjustStrength(dglab, TEST_CHANNEL_A, &g_test_strength_a, (int)TEST_STRENGTH_STEP, &rc))
        noteCommand("A up", rc, NULL);

    if (buttons & HidNpadButton_Down &&
        adjustStrength(dglab, TEST_CHANNEL_A, &g_test_strength_a, -(int)TEST_STRENGTH_STEP, &rc))
        noteCommand("A down", rc, NULL);

    if (buttons & HidNpadButton_Right &&
        adjustStrength(dglab, TEST_CHANNEL_B, &g_test_strength_b, (int)TEST_STRENGTH_STEP, &rc))
        noteCommand("B up", rc, NULL);

    if (buttons & HidNpadButton_Left &&
        adjustStrength(dglab, TEST_CHANNEL_B, &g_test_strength_b, -(int)TEST_STRENGTH_STEP, &rc))
        noteCommand("B down", rc, NULL);
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

static void handleButtons(Service* dglab, u64 down, u64 held, u64 now_ns)
{
    if (down & HidNpadButton_A) {
        DglabNetStartRequest request = { 0 };

        noteCommand("start", serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_START, request), NULL);
    }

    if (down & HidNpadButton_Y)
        noteCommand("stop", serviceDispatch(dglab, DGLAB_IPC_CMD_NET_STOP), NULL);

    if (down & HidNpadButton_B)
        noteCommand("clear", sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);

    // One trigger per channel: a waveform without a strength does nothing on the
    // device, and a strength without a waveform is just as silent, so each button
    // sends both for its own channel. A test at strength 0 is legal and answers
    // ok, so it says why nothing came out instead of leaving the screen alone.
    if (down & HidNpadButton_ZL)
        noteCommand("A test", testChannel(dglab, TEST_CHANNEL_A, g_test_strength_a),
            g_test_strength_a ? NULL : "A is 0");

    if (down & HidNpadButton_ZR)
        noteCommand("B test", testChannel(dglab, TEST_CHANNEL_B, g_test_strength_b),
            g_test_strength_b ? NULL : "B is 0");

    adjustStrengthFromDirections(dglab, down);
    repeatStrengthFromDirections(dglab, held, now_ns);
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

// Everything the screen draws, so the loop can tell whether redrawing is worth
// it. A full 1280x720 frame plus the QR code is expensive, and the panel only
// changes when one of these does.
typedef struct {
    DglabIpcVersion version;
    DglabNetStatus status;
    bool status_ok;
    bool url_ok;
    char url[DGLAB_NET_QR_MAX];
    u32 test_strength_a;
    u32 test_strength_b;
    char last_command[sizeof(g_last_command)];
    u32 last_command_tone;
    int log_count;
    u32 log_generation;
} DglabScreenSnapshot;

static void snapshotFromState(DglabScreenSnapshot* out, const DglabScreenState* state)
{
    memset(out, 0, sizeof(*out));

    out->version = state->version;
    out->status = state->status;
    out->status_ok = state->status_ok;
    out->url_ok = state->url_ok;
    out->test_strength_a = state->test_strength_a;
    out->test_strength_b = state->test_strength_b;
    out->last_command_tone = state->last_command_tone;
    out->log_count = state->log_count;
    out->log_generation = g_log_generation;

    snprintf(out->last_command, sizeof(out->last_command), "%s",
        state->last_command ? state->last_command : "");

    if (state->url)
        snprintf(out->url, sizeof(out->url), "%s", state->url);
}

// The socket test screen. Returns to the menu when `+` is pressed.
static void runSocketView(Service* dglab, PadState* pad)
{
    const DglabFont* font = dglabFramebufferFont();
    DglabIpcVersion version = { 0 };
    DglabScreenSnapshot snapshot;
    bool have_snapshot = false;
    char url[DGLAB_NET_QR_MAX];
    bool url_ok = false;
    u32 frame = 0;

    url[0] = '\0';

    serviceDispatchOut(dglab, DGLAB_IPC_CMD_GET_VERSION, version);

    while (appletMainLoop()) {
        DglabScreenState state;
        DglabNetQrChunk chunk;
        u64 down;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        memset(&state, 0, sizeof(state));
        memset(&chunk, 0, sizeof(chunk));

        state.version = version;
        state.url = url;
        state.url_ok = url_ok;

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

        if (down & HidNpadButton_Plus)
            return;

        handleButtons(dglab, down, padGetButtons(pad), armTicksToNs(armGetSystemTick()));

        // Read after the buttons are handled, so a press shows up in the same
        // frame it happened.
        state.test_strength_a = g_test_strength_a;
        state.test_strength_b = g_test_strength_b;
        state.last_command = g_last_command;
        state.last_command_tone = g_last_command_tone;

        DglabScreenSnapshot candidate;

        snapshotFromState(&candidate, &state);

        if (!have_snapshot || memcmp(&candidate, &snapshot, sizeof(candidate)) != 0) {
            DglabCanvas canvas;

            if (dglabFramebufferBegin(&canvas)) {
                dglabScreenDraw(&canvas, font, &state);
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

// How often the menu checks that the sysmodule is still there. Every mode needs
// it, and finding out after entering one wastes a trip through the menu.
#define MENU_PING_FRAMES 60

// Kept across visits, so leaving a mode comes back to the same entry.
static unsigned g_menu_selected = DglabMenu_ItemSocket;

static DglabMenuResult runMenuView(Service* dglab, PadState* pad)
{
    DglabMenuState state;
    unsigned drawn_selected = 0;
    bool drawn_liveness = false;
    bool have_drawn = false;
    u32 frame = 0;

    memset(&state, 0, sizeof(state));
    state.sysmodule_ok = true;

    while (appletMainLoop()) {
        DglabCanvas canvas;
        u64 down;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        if (down & HidNpadButton_Plus)
            return DglabMenuResult_Exit;

        if (down & HidNpadButton_Up)
            g_menu_selected = dglabMenuMove(g_menu_selected, -1);

        if (down & HidNpadButton_Down)
            g_menu_selected = dglabMenuMove(g_menu_selected, 1);

        if (down & HidNpadButton_A) {
            switch (g_menu_selected) {
                case DglabMenu_ItemMotion: return DglabMenuResult_Motion;
                case DglabMenu_ItemAdvanced: return DglabMenuResult_Advanced;
                case DglabMenu_ItemAbout: return DglabMenuResult_About;
                case DglabMenu_ItemBlePoc: return DglabMenuResult_BlePoc;
                default: return DglabMenuResult_Socket;
            }
        }

        if (++frame % MENU_PING_FRAMES == 1) {
            u32 pong = 0;

            state.sysmodule_ok =
                R_SUCCEEDED(serviceDispatchOut(dglab, DGLAB_IPC_CMD_PING, pong)) &&
                pong == DGLAB_IPC_PING_MAGIC;
        }

        state.selected = g_menu_selected;

        if (have_drawn && state.selected == drawn_selected &&
            state.sysmodule_ok == drawn_liveness)
            continue;

        if (dglabFramebufferBegin(&canvas)) {
            dglabMenuDraw(&canvas, g_text, &state);
            dglabFramebufferEnd();

            drawn_selected = state.selected;
            drawn_liveness = state.sysmodule_ok;
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

// Short form of the server's state for the motion screen's link line.
static const char* netStateText(u32 state)
{
    switch (state) {
        case DglabNetState_Listening: return "waiting for the app";
        case DglabNetState_Paired: return "app connected";
        case DglabNetState_Stopped: return "server stopped";
        case DglabNetState_Failed: return "server failed";
        default: return "server not started";
    }
}

static void runMotionView(Service* dglab, PadState* pad)
{
    const DglabFont* font = dglabFramebufferFont();
    DglabMotionFeedConfig config;
    DglabMotionFeed feed_a;
    DglabMotionFeed feed_b;
    DglabMotionScreenState state;
    DglabNetWaveformSlot slots[DGLAB_NET_WAVEFORM_MAX_SLOTS];
    u64 last_ticks;
    u32 frame = 0;

    motionSettingsLoad(&config);
    dglabMotionFeedInit(&feed_a, &config);
    dglabMotionFeedInit(&feed_b, &config);

    memset(&state, 0, sizeof(state));
    state.link = "server not started";
    state.last_upload = "";

    // Whatever the test buttons left queued should not play underneath the
    // motion stream.
    noteCommand("clear", sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);

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

        if (down & HidNpadButton_Plus)
            break;

        if (down & HidNpadButton_B)
            noteCommand("clear", sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);

        // Drain both sides every frame: the sensors run faster than this loop,
        // and a reading that is not collected now is gone.
        for (size_t i = 0, count = dglabJoyconPoll(DglabJoycon_Left, samples, MOTION_DRAIN_MAX);
             i < count; i++)
            dglabMotionFeedAddSample(&feed_a, &samples[i]);

        for (size_t i = 0, count = dglabJoyconPoll(DglabJoycon_Right, samples, MOTION_DRAIN_MAX);
             i < count; i++)
            dglabMotionFeedAddSample(&feed_b, &samples[i]);

        // A batch that is all silence is dropped instead of uploaded: that is
        // what makes a still controller cost no traffic at all.
        {
            size_t produced = dglabMotionFeedAdvance(&feed_a, elapsed_ns, slots,
                DGLAB_NET_WAVEFORM_MAX_SLOTS);

            if (produced > 0 && slotsHaveStrength(slots, produced))
                noteCommand("waveform A",
                    uploadSlots(dglab, MOTION_CHANNEL_A, slots, produced), NULL);
        }

        {
            size_t produced = dglabMotionFeedAdvance(&feed_b, elapsed_ns, slots,
                DGLAB_NET_WAVEFORM_MAX_SLOTS);

            if (produced > 0 && slotsHaveStrength(slots, produced))
                noteCommand("waveform B",
                    uploadSlots(dglab, MOTION_CHANNEL_B, slots, produced), NULL);
        }

        if (++frame % MOTION_DISPLAY_FRAMES == 1) {
            DglabNetStatus status;
            bool status_ok = R_SUCCEEDED(
                serviceDispatchOut(dglab, DGLAB_IPC_CMD_NET_STATUS, status));
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
            state.server_running = status_ok &&
                (status.state == DglabNetState_Listening || status.state == DglabNetState_Paired);

            if (!status_ok) {
                state.link = "IPC call failed";
                state.link_tone = DglabCmdTone_Error;
            } else {
                state.link = netStateText(status.state);
                state.link_tone = (status.state == DglabNetState_Paired) ? DglabCmdTone_Ok
                                                                        : DglabCmdTone_Warn;
            }

            state.last_upload = g_last_command;
            state.last_upload_tone = g_last_command_tone;

            if (dglabFramebufferBegin(&canvas)) {
                dglabMotionScreenDraw(&canvas, font, &state);
                dglabFramebufferEnd();
            }
        }
    }

    // Stop the stream before leaving: the sensors go quiet and the queued
    // waveform is cleared, so nothing keeps playing from the menu.
    dglabJoyconStop();
    noteCommand("clear", sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0), NULL);
}

// Error path: the console is used instead of the framebuffer, because it is the
// rendering path that is known to work on real hardware. Without the sysmodule
// there is nothing to draw anyway, and an empty window tells the user nothing.

// ---------------------------------------------------------------------------
// The about screen
// ---------------------------------------------------------------------------

static void runAboutView(Service* dglab, PadState* pad)
{
    DglabIpcVersion version = { 0 };
    DglabAboutState state;
    bool redraw = true;
    bool have_drawn = false;

    serviceDispatchOut(dglab, DGLAB_IPC_CMD_GET_VERSION, version);

    while (appletMainLoop()) {
        DglabCanvas canvas;
        u64 down;

        padUpdate(pad);
        down = padGetButtonsDown(pad);

        if (down & HidNpadButton_Plus)
            return;

        if ((down & HidNpadButton_Left) || (down & HidNpadButton_Right)) {
            // Left and right both cycle: with three values there is no natural
            // direction, and the row shows what it became.
            g_language_pref = dglabLanguageNext(g_language_pref);
            appLanguageSave();
            appLanguageApply();
            redraw = true;
        }

        if (!redraw && have_drawn)
            continue;

        memset(&state, 0, sizeof(state));
        state.preference = g_language_pref;
        state.resolved = g_language;
        state.version = version;
        state.github_url = "https://github.com/livcm/DGLAB-NX";

        if (dglabFramebufferBegin(&canvas)) {
            dglabAboutDraw(&canvas, g_text, &state);
            dglabFramebufferEnd();

            redraw = false;
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
    const DglabFont* font = dglabFramebufferFont();
    DglabMotionFeedConfig config;
    DglabAdvancedState state;
    u64 hold_started_ns = 0;
    u64 last_repeat_ns = 0;
    unsigned drawn_selected = ~0u;
    u32 revision = 0;
    u32 drawn_revision = ~0u;
    bool drawn_saved = false;

    (void)dglab;

    motionSettingsLoad(&config);

    memset(&state, 0, sizeof(state));
    state.config = &config;
    state.saved = true;

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

        if (down & HidNpadButton_Plus) {
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
            state.saved == drawn_saved)
            continue;

        if (dglabFramebufferBegin(&canvas)) {
            dglabAdvancedDraw(&canvas, font, &state);
            dglabFramebufferEnd();

            drawn_selected = state.selected;
            drawn_revision = revision;
            drawn_saved = state.saved;
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
    printf("    release/00FF072107210721/  ->  SD:/atmosphere/contents/00FF072107210721/\n");
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

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;


    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

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

    appLanguageLoad();
    appLanguageApply();

    while (true) {
        DglabMenuResult selection = runMenuView(&dglab, &pad);

        if (selection == DglabMenuResult_Exit)
            break;

        if (selection == DglabMenuResult_Socket) {
            runSocketView(&dglab, &pad);
            continue;
        }

        if (selection == DglabMenuResult_Motion) {
            runMotionView(&dglab, &pad);
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
        // framebuffer is released while it runs and created again afterwards.
        dglabFontClose();
    dglabFramebufferClose();
        dglabBlePocViewRun();

        if (!dglabFramebufferOpen())
            break;
    }

    dglabFramebufferClose();

    if (g_log_file != NULL)
        fclose(g_log_file);

    serviceClose(&dglab);

    return 0;
}
