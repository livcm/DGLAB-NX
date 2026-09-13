// DGLAB-NX front end.
//
// Two roles:
//   - the minimal IPC check that the sysmodule is reachable (GET_VERSION);
//   - a viewer and remote control for the BLE transport PoC running inside the
//     sysmodule, so the whole Bluetooth experiment is observable on the console
//     without a debugger.
//
// The NRO never talks to the Bluetooth stack itself: the sysmodule is the only
// owner of the DG-LAB connection.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include <dglab/ipc.h>
#include <dglab/ipc_poc.h>

#define LOG_LINES 12
#define LOG_LINE_LEN 72
#define LOG_POLL_ROUNDS 8
#define LOG_FILE_PATH "sdmc:/switch/dglab-ble-poc.log"

static char g_log_lines[LOG_LINES][LOG_LINE_LEN];
static int g_log_filled;
static u32 g_log_cursor;
static char g_partial[LOG_LINE_LEN];
static size_t g_partial_len;
static FILE* g_log_file;

static const char* pocStateName(u32 state)
{
    switch (state) {
        case DglabPocState_Idle: return "idle";
        case DglabPocState_Initializing: return "initializing";
        case DglabPocState_Scanning: return "scanning";
        case DglabPocState_Registering: return "registering";
        case DglabPocState_Connecting: return "connecting";
        case DglabPocState_Discovering: return "discovering";
        case DglabPocState_Ready: return "ready";
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

static void logFileOpen(void)
{
    if (R_FAILED(fsdevMountSdmc()))
        return;

    // The switch directory usually exists already; a failure here is not fatal.
    mkdir("sdmc:/switch", 0777);
    g_log_file = fopen(LOG_FILE_PATH, "w");
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

        if (c == '\r')
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
        { DGLAB_POC_MILESTONE_BLE_READY, "ble" },
        { DGLAB_POC_MILESTONE_SCAN_STARTED, "scan" },
        { DGLAB_POC_MILESTONE_DEVICE_FOUND, "found" },
        { DGLAB_POC_MILESTONE_CLIENT_READY, "client" },
        { DGLAB_POC_MILESTONE_CONNECTED, "conn" },
        { DGLAB_POC_MILESTONE_SERVICE_FOUND, "svc" },
        { DGLAB_POC_MILESTONE_CHARS_FOUND, "char" },
        { DGLAB_POC_MILESTONE_NOTIFY_ON, "notify" },
        { DGLAB_POC_MILESTONE_B0_WRITTEN, "b0" },
        { DGLAB_POC_MILESTONE_B1_RECEIVED, "b1" },
        { DGLAB_POC_MILESTONE_BATTERY_READ, "bat" },
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

    if (status->address_valid) {
        printf("device: %02X:%02X:%02X:%02X:%02X:%02X type=%u matched=%u\n", status->address[0],
            status->address[1], status->address[2], status->address[3], status->address[4],
            status->address[5], status->ble_addr_type, status->scan_matched);
    } else {
        printf("device: none  scan results: %u\n", status->scan_results);
    }

    printf("client_if: %u  conn_id: %u  mtu: %u\n", status->client_if, status->conn_id,
        status->mtu);

    printf("char props: write=0x%02X notify=0x%02X battery=0x%02X\n", status->char_write_prop,
        status->char_notify_prop, status->char_battery_prop);

    printf("b0 writes: %u (failed %u)  auto=%u\n", status->b0_write_count,
        status->b0_write_failures, status->auto_write);

    printf("notifications: %u  battery: ", status->notify_count);

    if (status->battery_valid)
        printf("%u\n", status->battery_value);
    else
        printf("none\n");

    printf("last notify: ");

    if (status->last_notify_size) {
        u32 size = status->last_notify_size;

        if (size > sizeof(status->last_notify))
            size = sizeof(status->last_notify);

        for (u32 i = 0; i < size; i++)
            printf("%02X", status->last_notify[i]);

        printf("\n");
    } else {
        printf("none\n");
    }
}

static void printLog(void)
{
    printf("------------------------ log -------------------------\n");

    for (int i = 0; i < g_log_filled; i++)
        printf("%s\n", g_log_lines[LOG_LINES - g_log_filled + i]);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    consoleInit(NULL);
    logFileOpen();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Service dglab;
    Result rc = smGetService(&dglab, DGLAB_IPC_SERVICE_NAME);

    if (R_FAILED(rc)) {
        printf("DGLAB sysmodule not found (0x%08X)\n", rc);
        printf("Install the sysmodule and reboot the console.\n");
        printf("\nPress + to exit.\n");
        consoleUpdate(NULL);

        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            consoleUpdate(NULL);
        }

        consoleExit(NULL);
        return 0;
    }

    DglabIpcVersion version = { 0 };
    rc = serviceDispatchOut(&dglab, DGLAB_IPC_CMD_GET_VERSION, version);
    if (R_FAILED(rc)) {
        printf("GetVersion failed (0x%08X)\n", rc);
        serviceClose(&dglab);
        consoleExit(NULL);
        return 0;
    }

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus)
            break;

        if (down & HidNpadButton_A) {
            DglabPocStartRequest request = { 0 };
            request.applet_resource_user_id = appletGetAppletResourceUserId();
            serviceDispatchIn(&dglab, DGLAB_IPC_POC_CMD_START, request);
        }

        if (down & HidNpadButton_Minus)
            serviceDispatch(&dglab, DGLAB_IPC_POC_CMD_STOP);

        if (down & HidNpadButton_X) {
            DglabPocActionRequest request = { 0 };
            request.action = DglabPocAction_WriteZeroB0;
            serviceDispatchIn(&dglab, DGLAB_IPC_POC_CMD_ACTION, request);
        }

        if (down & HidNpadButton_B) {
            DglabPocActionRequest request = { 0 };
            request.action = DglabPocAction_ReadBattery;
            serviceDispatchIn(&dglab, DGLAB_IPC_POC_CMD_ACTION, request);
        }

        if (down & HidNpadButton_Y) {
            DglabPocActionRequest request = { 0 };
            request.action = DglabPocAction_Disconnect;
            serviceDispatchIn(&dglab, DGLAB_IPC_POC_CMD_ACTION, request);
        }

        if (down & HidNpadButton_R) {
            DglabPocActionRequest request = { 0 };
            request.action = DglabPocAction_ReconnectAruid0;
            serviceDispatchIn(&dglab, DGLAB_IPC_POC_CMD_ACTION, request);
        }

        if (down & HidNpadButton_L) {
            DglabPocActionRequest request = { 0 };
            request.action = DglabPocAction_ToggleAutoWrite;
            serviceDispatchIn(&dglab, DGLAB_IPC_POC_CMD_ACTION, request);
        }

        DglabPocStatus status;
        memset(&status, 0, sizeof(status));
        rc = serviceDispatchOut(&dglab, DGLAB_IPC_POC_CMD_STATUS, status);

        if (R_SUCCEEDED(rc))
            pocPollLog(&dglab);

        printf("\x1b[2J\x1b[H");
        printf("DGLAB-NX BLE PoC   (IPC %u.%u.%u)\n\n", version.major, version.minor,
            version.patch);

        if (R_SUCCEEDED(rc))
            printStatus(&status);
        else
            printf("PoC status failed (0x%08X)\n", rc);

        printf("\n");
        printLog();
        printf("A start  X zero-B0  B battery  R aruid0  L auto  Y disconn  - stop  + exit\n");

        consoleUpdate(NULL);
    }

    serviceClose(&dglab);

    if (g_log_file != NULL)
        fclose(g_log_file);

    consoleExit(NULL);
    return 0;
}
