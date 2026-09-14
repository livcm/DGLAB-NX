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
#include <string.h>

#include <switch.h>

#include <dglab/ipc.h>
#include <dglab/nro/ble_poc_view.h>
#include <dglab/platform/framebuffer.h>
#include <dglab/ui/screen.h>

#define LOG_POLL_ROUNDS 2

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
static u32 g_test_strength = 10;

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

static void handleButtons(Service* dglab, u64 down)
{
    if (down & HidNpadButton_A) {
        DglabNetStartRequest request = { 0 };

        serviceDispatchIn(dglab, DGLAB_IPC_CMD_NET_START, request);
    }

    if (down & HidNpadButton_Y)
        serviceDispatch(dglab, DGLAB_IPC_CMD_NET_STOP);

    if (down & HidNpadButton_X)
        sendTestCommand(dglab, DglabNetCommand_SetStrength, 0, g_test_strength);

    if (down & HidNpadButton_B)
        sendTestCommand(dglab, DglabNetCommand_Clear, 0, 0);

    if (down & HidNpadButton_ZL)
        sendTestCommand(dglab, DglabNetCommand_TestPulse, 0, g_test_strength);

    if (down & HidNpadButton_L)
        g_test_strength = (g_test_strength > 5) ? g_test_strength - 5 : 100;

    if (down & HidNpadButton_R)
        g_test_strength = (g_test_strength < 100) ? g_test_strength + 5 : 5;
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

static DglabViewResult runSocketView(Service* dglab, bool service_ready, PadState* pad)
{
    const DglabFont* font = dglabFramebufferFont();
    DglabIpcVersion version = { 0 };
    char url[DGLAB_NET_QR_MAX];
    bool url_ok = false;

    url[0] = '\0';

    if (service_ready)
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
        state.service_ready = service_ready;
        state.test_strength = g_test_strength;
        state.log_lines = g_log_pointers;
        state.log_count = g_log_filled;
        state.url = url;
        state.url_ok = url_ok;

        if (service_ready) {
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
        }

        if (down & HidNpadButton_Plus)
            return DglabView_Exit;

        if (down & HidNpadButton_Minus)
            return DglabView_BlePoc;

        if (service_ready)
            handleButtons(dglab, down);

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

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    Service dglab;
    bool service_ready = R_SUCCEEDED(smGetService(&dglab, DGLAB_IPC_SERVICE_NAME));

    if (!dglabFramebufferOpen()) {
        if (service_ready)
            serviceClose(&dglab);

        return 0;
    }

    while (true) {
        DglabViewResult result = runSocketView(&dglab, service_ready, &pad);

        if (result == DglabView_Exit)
            break;

        // The BLE PoC view takes the console and the screen over, so the
        // framebuffer is released while it runs and created again afterwards.
        dglabFramebufferClose();
        dglabBlePocViewRun();

        if (!dglabFramebufferOpen())
            break;
    }

    dglabFramebufferClose();

    if (service_ready)
        serviceClose(&dglab);

    return 0;
}
