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
#include <dglab/nro/ble_poc_view.h>
#include <dglab/platform/framebuffer.h>
#include <dglab/ui/screen.h>

#define LOG_POLL_ROUNDS 2

// The sysmodule log is mirrored to the SD card, so a test run can be reported
// back as a file instead of as a photo of the screen.
#define DATA_DIR "sdmc:/switch/DGLAB-NX"
#define LOG_FILE_PATH DATA_DIR "/dglab-net.log"

typedef enum {
    DglabView_Exit = 0,
    DglabView_BlePoc,
} DglabViewResult;

static char g_log_lines[DGLAB_SCREEN_LOG_LINES][DGLAB_SCREEN_LOG_LINE_LEN];
static const char* g_log_pointers[DGLAB_SCREEN_LOG_LINES];
static int g_log_filled;
static u32 g_log_cursor;
static char g_partial[256];
static size_t g_partial_len;
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

// Held buttons repeat at a usable rate; the UI loop runs at the display refresh.
#define TEST_STRENGTH_REPEAT_FRAMES 6
static u32 g_test_strength_a = TEST_STRENGTH_MIN;
static u32 g_test_strength_b = TEST_STRENGTH_MIN;
static int g_test_strength_held_frames;
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

static void sendTestCommand(Service* dglab, u32 command, u32 channel, u32 value)
{
    DglabNetSendRequest request = { 0 };

    request.command = command;
    request.channel = channel;
    request.value = value;

    serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_SEND, request);
}

// Queues one second of test waveform through the streaming path: 48 slots of
// 25ms each, at the app side frequency of 100ms, with the waveform at full
// strength so the channel strength alone decides how strong it feels. The
// waveform goes out on one channel; Replace mode makes every press start a fresh
// gesture rather than queueing behind the previous one.
static void sendTestWaveform(Service* dglab, u32 channel)
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

    serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_WAVEFORM, request);
}

// Fires one channel: the waveform plus that channel's strength, so the button is
// self contained even if the App reconnected since the strength last changed.
static void testChannel(Service* dglab, u32 channel, u32 strength)
{
    sendTestWaveform(dglab, channel);
    sendTestCommand(dglab, DglabNetCommand_SetStrength, channel, strength);
}

// Steps one channel's strength and sends it straight away - there is no separate
// "send strength" button. Clamped instead of wrapping: at 0 a decrease does
// nothing, at 100 an increase does nothing, and a clamped step sends nothing.
static void adjustStrength(Service* dglab, u32 channel, u32* value, int delta)
{
    int next = (int)*value + delta;

    if (next < (int)TEST_STRENGTH_MIN)
        next = (int)TEST_STRENGTH_MIN;

    if (next > (int)TEST_STRENGTH_MAX)
        next = (int)TEST_STRENGTH_MAX;

    if ((u32)next == *value)
        return;

    *value = (u32)next;
    sendTestCommand(dglab, DglabNetCommand_SetStrength, channel, *value);
}

// The D-pad is the mixer: the vertical axis dials channel A, the horizontal one
// dials channel B. Used for the first press and, at a slower rate, while held.
static void adjustStrengthFromDirections(Service* dglab, u64 buttons)
{
    if (buttons & HidNpadButton_Up)
        adjustStrength(dglab, TEST_CHANNEL_A, &g_test_strength_a, (int)TEST_STRENGTH_STEP);

    if (buttons & HidNpadButton_Down)
        adjustStrength(dglab, TEST_CHANNEL_A, &g_test_strength_a, -(int)TEST_STRENGTH_STEP);

    if (buttons & HidNpadButton_Right)
        adjustStrength(dglab, TEST_CHANNEL_B, &g_test_strength_b, (int)TEST_STRENGTH_STEP);

    if (buttons & HidNpadButton_Left)
        adjustStrength(dglab, TEST_CHANNEL_B, &g_test_strength_b, -(int)TEST_STRENGTH_STEP);
}

static void handleButtons(Service* dglab, u64 down, u64 held)
{
    if (down & HidNpadButton_A) {
        DglabNetStartRequest request = { 0 };

        serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_START, request);
    }

    if (down & HidNpadButton_Y)
        serviceDispatch(dglab, DGLAB_IPC_CMD_NET_STOP);

    if (down & HidNpadButton_B)
        sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0);

    // One trigger per channel: a waveform without a strength does nothing on the
    // device, and a strength without a waveform is just as silent, so each button
    // sends both for its own channel.
    if (down & HidNpadButton_ZL)
        testChannel(dglab, TEST_CHANNEL_A, g_test_strength_a);

    if (down & HidNpadButton_ZR)
        testChannel(dglab, TEST_CHANNEL_B, g_test_strength_b);

    adjustStrengthFromDirections(dglab, down);

    // Holding a direction walks the value at a usable speed instead of repeating
    // at the display refresh rate.
    if (held & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Right |
            HidNpadButton_Left)) {
        g_test_strength_held_frames++;

        if (g_test_strength_held_frames >= TEST_STRENGTH_REPEAT_FRAMES) {
            g_test_strength_held_frames = 0;

            adjustStrengthFromDirections(dglab, held);
        }
    } else {
        g_test_strength_held_frames = 0;
    }
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

static DglabViewResult runSocketView(Service* dglab, PadState* pad)
{
    const DglabFont* font = dglabFramebufferFont();
    DglabIpcVersion version = { 0 };
    char url[DGLAB_NET_QR_MAX];
    bool url_ok = false;

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
        state.test_strength_a = g_test_strength_a;
        state.test_strength_b = g_test_strength_b;
        state.log_lines = g_log_pointers;
        state.log_count = g_log_filled;
        state.url = url;
        state.url_ok = url_ok;

        state.status_ok =
            R_SUCCEEDED(serviceDispatchOut(dglab, DGLAB_IPC_CMD_NET_STATUS, state.status));

        logPoll(dglab);
        buildLogPointers();

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
            return DglabView_Exit;

        if (down & HidNpadButton_Minus)
            return DglabView_BlePoc;

        handleButtons(dglab, down, padGetButtons(pad));

        {
            DglabCanvas canvas;

            if (dglabFramebufferBegin(&canvas)) {
                dglabScreenDraw(&canvas, font, &state);
                dglabFramebufferEnd();
            }
        }
    }

    return DglabView_Exit;
}

// Error path: the console is used instead of the framebuffer, because it is the
// rendering path that is known to work on real hardware. Without the sysmodule
// there is nothing to draw anyway, and an empty window tells the user nothing.
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

    while (true) {
        DglabViewResult result = runSocketView(&dglab, &pad);

        if (result == DglabView_Exit) {
            break;
        }

        // The BLE PoC view takes the console and the screen over, so the
        // framebuffer is released while it runs and created again afterwards.
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
