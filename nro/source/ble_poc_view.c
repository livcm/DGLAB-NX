// DGLAB-NX BLE transport PoC view.
//
// Two roles:
//   - the minimal IPC check that the sysmodule is reachable (GET_VERSION);
//   - a viewer and remote control for the BLE transport PoC running inside the
//     sysmodule, so the whole Bluetooth experiment is observable on the console
//     without a debugger.
//
// The NRO never talks to the Bluetooth stack itself: the sysmodule is the only
// owner of the DG-LAB connection.
//
// The Bluetooth route is shelved (see docs/ble-poc.md), so this console view is
// only a diagnostic tool now. The main entry point opens it through
// dglabBlePocViewRun().

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include <dglab/ipc.h>
#include <dglab/ipc_poc.h>
#include <dglab/nro/ble_poc_view.h>

// The on screen ring keeps short lines for layout, but the file gets the whole
// line: the sysmodule log lines carry filter patterns and UUIDs at the end, and
// truncating them to the display width hid exactly that information.
#define LOG_LINES 12
#define LOG_LINE_LEN 72
#define LOG_LINE_MAX 192
#define LOG_POLL_ROUNDS 8

// The same SD card layout the rest of the front end uses (main.c): this view
// reads its optional target address from config/ and writes its log to logs/.
#define DATA_DIR "sdmc:/switch/DGLAB-NX"
#define CONFIG_DIR DATA_DIR "/config"
#define LOG_DIR DATA_DIR "/logs"
#define LOG_FILE_PATH LOG_DIR "/dglab-ble-poc.log"
// Optional: connect straight to this address instead of scanning, for the case
// where the console's scan filters cannot report the device.
#define ADDRESS_FILE_PATH CONFIG_DIR "/dglab-ble-address.txt"

static char g_log_lines[LOG_LINES][LOG_LINE_LEN];
static int g_log_filled;
static u32 g_log_cursor;
static char g_partial[LOG_LINE_MAX];
static size_t g_partial_len;
static FILE* g_log_file;
static u8 g_target_address[6];
static bool g_target_address_valid;

// Steps of the one-key probe sequence: 0 = idle, 1 = the driver-level probe is
// running (or starting), 2 = the btm probe is running. The next session is
// started when the previous one has finished.
static u32 g_probe_sequence;
static u32 g_probe_idle_frames;

// The next session starts only after the previous one has looked idle for this
// many consecutive frames. One frame is not enough: the status can still say
// "stopped" (or the read can fail) while the worker is finishing its last
// connects, and starting the btm session on top of that is what left the
// 19:0x round with result=0x1A - the same failure as before the exefs patch
// (docs/ble-re.md, "一个开机周期只走一条 BLE 路径").
#define PROBE_SEQUENCE_IDLE_FRAMES 30u

static const char* pocStateName(u32 state)
{
    switch (state) {
        case DglabPocState_Idle: return "idle";
        case DglabPocState_Initializing: return "initializing";
        case DglabPocState_Failed: return "FAILED";
        case DglabPocState_Stopped: return "stopped";
        default: return "?";
    }
}

static void logPushLine(const char* line)
{
    size_t len = strlen(line);

    if (LOG_LINES > 1)
        memmove(g_log_lines[0], g_log_lines[1], sizeof(g_log_lines[0]) * (LOG_LINES - 1));

    if (len >= LOG_LINE_LEN)
        len = LOG_LINE_LEN - 1;

    memcpy(g_log_lines[LOG_LINES - 1], line, len);
    g_log_lines[LOG_LINES - 1][len] = '\0';

    if (g_log_filled < LOG_LINES)
        g_log_filled++;

    // Mirror the sysmodule log to the SD card so a test run can be reported
    // back without photographing the console.
    if (g_log_file != NULL) {
        fprintf(g_log_file, "%s\n", line);
        fflush(g_log_file);
    }
}

void dglabBlePocViewLogLine(const char* line)
{
    logPushLine(line);
}

// Creates the SD card directories if they are not there yet. A failure here is
// not fatal: they are usually already present, and if the card really is
// unusable the fopen below reports it.
static void ensureDataDir(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(DATA_DIR, 0777);
    mkdir(CONFIG_DIR, 0777);
    mkdir(LOG_DIR, 0777);
}

// libnx's default init already mounts sdmc, so mounting again must never be
// treated as a hard failure: that would silently drop the whole log. Just try
// to open the file and report what actually happened.
static bool logFileOpen(void)
{
    ensureDataDir();
    g_log_file = fopen(LOG_FILE_PATH, "w");

    if (g_log_file == NULL) {
        fsdevMountSdmc();
        ensureDataDir();
        g_log_file = fopen(LOG_FILE_PATH, "w");
    }

    if (g_log_file == NULL)
        g_log_file = fopen("sdmc:/dglab-ble-poc.log", "w");

    return g_log_file != NULL;
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

        // Skip padding left over from a cleared sysmodule log ring.
        if (c == '\r' || c == '\0')
            continue;

        if (g_partial_len + 1 < sizeof(g_partial))
            g_partial[g_partial_len++] = c;
    }
}

static Result pocPollLog(Service* dglab)
{
    for (int i = 0; i < LOG_POLL_ROUNDS; i++) {
        DglabPocLogRequest request = { 0 };
        DglabPocLogChunk chunk;
        Result rc;

        request.cursor = g_log_cursor;
        memset(&chunk, 0, sizeof(chunk));

        rc = serviceDispatchInOut(dglab, DGLAB_IPC_POC_CMD_LOG, request, chunk);
        if (R_FAILED(rc))
            return rc;

        if (chunk.size) {
            u32 size = chunk.size;

            if (size > sizeof(chunk.text))
                size = sizeof(chunk.text);

            logAppend(chunk.text, size);
        }

        if (chunk.next_cursor == g_log_cursor)
            break;

        g_log_cursor = chunk.next_cursor;

        if (chunk.size == 0)
            break;
    }

    return 0;
}

static void printMilestones(u32 milestone)
{
    static const struct {
        u32 bit;
        const char* name;
    } kItems[] = {
        { DGLAB_POC_MILESTONE_DEVICE_FOUND, "found" },
        { DGLAB_POC_MILESTONE_CONNECTED, "conn" },
        { DGLAB_POC_MILESTONE_SERVICE_FOUND, "svc" },
    };

    for (size_t i = 0; i < sizeof(kItems) / sizeof(kItems[0]); i++)
        printf("%s%s ", (milestone & kItems[i].bit) ? "+" : ".", kItems[i].name);
}

static void printStatus(const DglabPocStatus* status)
{
    printf("state: %-12s  aruid: 0x%08X\n", pocStateName(status->state), status->aruid_low);
    printf("last result: 0x%08X\n", status->last_result);

    printf("milestones: ");
    printMilestones(status->milestone);
    printf("\n");
}

static void printLog(void)
{
    printf("------------------------ log -------------------------\n");

    for (int i = 0; i < g_log_filled; i++)
        printf("%s\n", g_log_lines[LOG_LINES - g_log_filled + i]);
}

// Reads an optional "XX:XX:XX:XX:XX:XX" file so the PoC can skip the console's
// scan filters and connect straight to a known address.
static bool loadTargetAddress(u8 out[6])
{
    FILE* file = fopen(ADDRESS_FILE_PATH, "r");
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

// Idle, failed and stopped all mean the sysmodule has no worker running, so a
// new run has to be started before an action can be accepted.
static bool pocStateIsActive(u32 state)
{
    switch (state) {
        case DglabPocState_Idle:
        case DglabPocState_Failed:
        case DglabPocState_Stopped:
            return false;
        default:
            return true;
    }
}

// Every action the console can send is a probe, and each probe runs a session of
// its own (docs/history.md §28: the driver-level probe brings the BLE stack up,
// and the btm probe must be the first Bluetooth access of its session).
static Result pocSendStart(Service* dglab)
{
    DglabPocStartRequest request = { 0 };

    request.applet_resource_user_id = appletGetAppletResourceUserId();

    g_target_address_valid = loadTargetAddress(request.target_address);

    if (g_target_address_valid) {
        request.flags |= DGLAB_POC_START_FLAG_TARGET_ADDRESS;
        memcpy(g_target_address, request.target_address, sizeof(g_target_address));
    }

    return serviceDispatchIn(dglab, DGLAB_IPC_POC_CMD_START, request);
}

static Result pocSendAction(Service* dglab, u32 action)
{
    DglabPocActionRequest request = { 0 };

    request.action = action;
    return serviceDispatchIn(dglab, DGLAB_IPC_POC_CMD_ACTION, request);
}

void dglabBlePocViewRun(Service* dglab)
{
    consoleInit(NULL);

    // Print something before touching the filesystem or IPC, so that a stall
    // shows up on screen instead of looking like a dead NRO.
    printf("DGLAB-NX BLE PoC\n\n");
    printf("console ready\n");
    consoleUpdate(NULL);

    if (logFileOpen())
        printf("log: %s\n", LOG_FILE_PATH);
    else
        printf("log: unavailable, nothing will be saved\n");

    consoleUpdate(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("querying sysmodule...\n");
    consoleUpdate(NULL);

    // The session belongs to the caller (see the header): this view must not
    // open its own, because the sysmodule serves one session at a time.
    DglabIpcVersion version = { 0 };
    Result rc = serviceDispatchOut(dglab, DGLAB_IPC_CMD_GET_VERSION, version);
    if (R_FAILED(rc)) {
        printf("GetVersion failed (0x%08X)\n", rc);
        printf("The sysmodule stopped while this view was open.\n");
        printf("\nPress + to exit.\n");
        consoleUpdate(NULL);

        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            consoleUpdate(NULL);
        }

        consoleExit(NULL);
        return;
    }

    DglabPocStatus status;
    Result status_rc = 0;

    // The key that opened this page - A in the menu - is still reported as
    // "pressed down" by the first padUpdate here, and that used to start a
    // session (and with it the identity probe) before the user touched
    // anything. Swallow the buttons of the first frame.
    bool swallow_first_frame = true;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (swallow_first_frame) {
            swallow_first_frame = false;
            down = 0;
        }

        // Refresh the status before handling buttons: the action handling needs
        // to know whether a run is active, because the sysmodule drops actions
        // when no worker is running.
        memset(&status, 0, sizeof(status));
        status_rc = serviceDispatchOut(dglab, DGLAB_IPC_POC_CMD_STATUS, status);

        if (R_SUCCEEDED(status_rc))
            pocPollLog(dglab);

        bool run_active = R_SUCCEEDED(status_rc) && pocStateIsActive(status.state);

        // One key, two sessions. The driver-level probe has to run in its own
        // session first (it is what brings the BLE stack up; doing that inside
        // the btm session makes the connect fail, docs/ble-re.md), and the btm
        // probe runs in the next one. The sequence is stepped here so the user
        // presses the key once.
        if (!run_active && g_probe_sequence != 0u) {
            g_probe_idle_frames++;
        } else if (g_probe_sequence != 0u) {
            g_probe_idle_frames = 0;
        }

        if (g_probe_sequence != 0u && g_probe_idle_frames >= PROBE_SEQUENCE_IDLE_FRAMES) {
            if (g_probe_sequence == 1u) {
                logPushLine("one-key probe: driver-level probe done, starting the btm probe");
                pocSendStart(dglab);
                pocSendAction(dglab, DglabPocAction_ProbeBtmBle);
                g_probe_sequence = 2u;
            } else {
                logPushLine("one-key probe: done - reboot before the next experiment");
                g_probe_sequence = 0u;
            }

            g_probe_idle_frames = 0;
        }

        if (down & HidNpadButton_Plus)
            break;

        if (down & HidNpadButton_Minus)
            serviceDispatch(dglab, DGLAB_IPC_POC_CMD_STOP);

        u32 action = 0;

        // One key starts the whole sequence - the driver-level probe first, then
        // the base-btm probe. A only: it is the key that opened this page, it
        // cannot be confused with the stick buttons, and one trigger means there
        // is exactly one way to start a round (docs/ble-re.md, "一个开机周期只走
        // 一条 BLE 路径"). D-pad Left still runs the driver-level probe on its own.
        if ((down & HidNpadButton_A) && !run_active) {
            pocSendStart(dglab);
            pocSendAction(dglab, DglabPocAction_ProbeBtdrvScan);
            g_probe_sequence = 1u;
            logPushLine("one-key probe: step 1/2, driver-level probe (brings the BLE stack up)");
        } else if (down & HidNpadButton_Left) {
            action = DglabPocAction_ProbeBtdrvScan;
        }

        if (action != 0) {
            // Start a run first when idle so a single button press works from
            // the idle screen.
            if (!run_active)
                pocSendStart(dglab);

            pocSendAction(dglab, action);
        }

        consoleClear();
        printf("DGLAB-NX BLE PoC   (IPC %u.%u.%u)\n\n", version.major, version.minor,
            version.patch);

        if (R_SUCCEEDED(status_rc))
            printStatus(&status);
        else
            printf("PoC status failed (0x%08X)\n", status_rc);

        printf("\n");
        printLog();

        // The target address is read from the SD card once, so the display can
        // say where the next session will connect before it starts.
        if (!g_target_address_valid)
            g_target_address_valid = loadTargetAddress(g_target_address);

        if (g_target_address_valid) {
            printf("target: %02X:%02X:%02X:%02X:%02X:%02X (from %s)\n", g_target_address[0],
                g_target_address[1], g_target_address[2], g_target_address[3],
                g_target_address[4], g_target_address[5], ADDRESS_FILE_PATH);
        } else {
            printf("target: none, scanning (see %s)\n", ADDRESS_FILE_PATH);
        }

        // What to press goes first; the key list is short by design (the probes
        // that answered their question were removed, see docs/history.md).
        printf(">>> PRESS A while idle: one-key probe <<<\n");
        printf("    1) driver-level probe (brings the BLE stack up)\n");
        printf("    2) base btm probe (scan, connect, GATT, BF/B0 + reaction test)\n");
        printf("    it leaves btm busy: reboot before the next experiment\n");
        printf("\n");
        printf("A            one-key probe   D-pad Left  driver-level probe only\n");
        printf("-  stop the session          +  exit\n");

        consoleUpdate(NULL);
    }

    if (g_log_file != NULL)
        fclose(g_log_file);

    consoleExit(NULL);
}
