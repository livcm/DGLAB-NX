// BLE transport proof of concept.
//
// This is the only place in the project that talks to the Switch Bluetooth
// stack, and it runs in the sysmodule: the NRO never touches BLE itself.
//
// Transport choice:
//   btdrv's own BLE event queue was the first attempt, but on HOS 22.5.0 it
//   only ever produced empty payloads (documented in docs/ble-poc.md). libnx's
//   btdev wrapper (bt + btm:u) works from this background process and hides the
//   event/type plumbing, so the PoC uses btdev for everything.
//
// Scan filter:
//   The Coyote 3.0 advertises the HID service UUID 0x1812. The DG-LAB service
//   0x180C only exists after connecting, so a scan filtered by 0x180C finds
//   nothing. The scan therefore tries 0x1812 first and falls back to 0x180C.
//
// Packet construction uses dglab/protocol/coyote_v3.h; this file only moves
// bytes and reports what happened through the log ring the NRO reads.

#include <dglab/transport/ble_poc.h>

#include <dglab/protocol/coyote_v3.h>

#include <switch/runtime/btdev.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

#define POC_THREAD_STACK_SIZE 0x8000u
#define POC_LOG_CAPACITY 4096u
#define POC_LOG_LINE_MAX 160u

#define POC_SCAN_TIMEOUT_MS 8000u
#define POC_CONNECT_TIMEOUT_MS 12000u
#define POC_DISCOVER_TIMEOUT_MS 8000u
#define POC_SCAN_ATTEMPTS 3u
#define POC_B0_INTERVAL_MS 100u
#define POC_NOTIFY_LOG_LIMIT 8u
#define POC_SCAN_POLL_LOG_EVERY 10u

// Service UUID the Coyote 3.0 puts into its advertisement. Reported by scanning
// the device with a phone BLE scanner; see docs/ble-poc.md.
#define POC_UUID16_ADVERTISED_SERVICE 0x1812u

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct {
    bool ble_ready;

    Event scan_event;
    Event conn_event;
    Event discovery_event;
    Event gatt_event;
    bool scan_event_active;
    bool conn_event_active;
    bool discovery_event_active;
    bool gatt_event_active;

    bool scanning;
    bool connected;
    u32 connection_handle;
    BtdrvAddress address;
    u16 filter_used;
    u16 forced_filter; // 0 = default order, otherwise scan only with this UUID
    bool restart_scan;
    bool probe_btdrv;
    u32 scan_attempts;

    BtdevGattService service;             // 0x180C
    BtdevGattCharacteristic char_write;   // 0x150A
    BtdevGattCharacteristic char_notify;  // 0x150B
    BtdevGattService battery_service;     // 0x180A
    BtdevGattCharacteristic char_battery; // 0x1500
    bool have_write;
    bool have_notify;
    bool have_battery;

    u32 next_b0_ms;
    u32 notify_logged;
    u32 scan_polls;
    u32 scan_events;
    u32 gatt_polls;
} PocWorker;

typedef struct {
    Mutex mutex;

    // Guarded by mutex.
    DglabPocStatus status;
    u32 log_write_offset;
    u32 log_valid_from;
    char log[POC_LOG_CAPACITY];
    Thread worker_thread;
    bool running;
    bool stop_requested;
    bool auto_write;
    u64 aruid;
    u32 pending_action;
    bool use_target_address;
    u8 target_address[6];
    u32 start_scan_filter;

    // Worker owned.
    PocWorker worker;
} PocShared;

static PocShared g_poc;

// The worker thread is created on demand, so its stack cannot live on the stack
// of whichever thread starts it.
static u8 g_poc_thread_stack[POC_THREAD_STACK_SIZE] __attribute__((aligned(0x1000)));

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static u32 pocNowMs(void)
{
    return (u32)(armTicksToNs(armGetSystemTick()) / 1000000ull);
}

static bool pocStopRequested(void)
{
    bool stop;

    mutexLock(&g_poc.mutex);
    stop = g_poc.stop_requested;
    mutexUnlock(&g_poc.mutex);

    return stop;
}

// Builds the 128-bit form of a 16-bit Bluetooth UUID using the base UUID from
// the DG-LAB documentation. The full form avoids any question about the byte
// order of the 2-byte form.
static BtdrvGattAttributeUuid pocUuid16(u16 value)
{
    static const u8 base[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB,
    };
    BtdrvGattAttributeUuid uuid;

    memset(&uuid, 0, sizeof(uuid));
    uuid.size = 0x10;
    memcpy(uuid.uuid, base, sizeof(base));
    uuid.uuid[2] = (u8)(value >> 8);
    uuid.uuid[3] = (u8)(value & 0xFF);

    return uuid;
}

static void pocHex(char* out, size_t out_size, const u8* data, size_t size)
{
    size_t used = 0;

    if (out_size == 0)
        return;

    out[0] = '\0';

    for (size_t i = 0; i < size && used + 3 < out_size; i++)
        used += (size_t)snprintf(out + used, out_size - used, "%02X", data[i]);
}

static const char* pocStateName(u32 state)
{
    switch (state) {
        case DglabPocState_Idle: return "idle";
        case DglabPocState_Initializing: return "init";
        case DglabPocState_Scanning: return "scanning";
        case DglabPocState_Registering: return "registering";
        case DglabPocState_Connecting: return "connecting";
        case DglabPocState_Discovering: return "discovering";
        case DglabPocState_Ready: return "ready";
        case DglabPocState_Failed: return "failed";
        case DglabPocState_Stopped: return "stopped";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Logging and status
// ---------------------------------------------------------------------------

static void pocLog(const char* fmt, ...)
{
    char line[POC_LOG_LINE_MAX];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);

    if (len < 0)
        return;

    if ((size_t)len > sizeof(line) - 2)
        len = (int)(sizeof(line) - 2);

    line[len] = '\n';
    len++;

    mutexLock(&g_poc.mutex);
    for (int i = 0; i < len; i++) {
        g_poc.log[g_poc.log_write_offset % POC_LOG_CAPACITY] = line[i];
        g_poc.log_write_offset++;
    }
    mutexUnlock(&g_poc.mutex);
}

static void pocSetState(u32 state)
{
    mutexLock(&g_poc.mutex);
    bool changed = g_poc.status.state != state;
    g_poc.status.state = state;
    mutexUnlock(&g_poc.mutex);

    if (changed)
        pocLog("state=%s", pocStateName(state));
}

static void pocSetMilestone(u32 bits)
{
    mutexLock(&g_poc.mutex);
    g_poc.status.milestone |= bits;
    mutexUnlock(&g_poc.mutex);
}

static void pocFail(Result rc, const char* what)
{
    pocLog("FAIL %s rc=0x%08X", what, (u32)rc);

    mutexLock(&g_poc.mutex);
    g_poc.status.last_result = (u32)rc;
    g_poc.status.state = DglabPocState_Failed;
    mutexUnlock(&g_poc.mutex);
}

static void pocSetCharProperty(u8* field, u8 value)
{
    mutexLock(&g_poc.mutex);
    *field = value;
    mutexUnlock(&g_poc.mutex);
}

static void pocRecordScanResult(void)
{
    mutexLock(&g_poc.mutex);
    g_poc.status.scan_results++;
    mutexUnlock(&g_poc.mutex);
}

static void pocRecordMatch(const BtdrvAddress* addr, u16 filter)
{
    mutexLock(&g_poc.mutex);
    g_poc.status.scan_matched++;
    memcpy(g_poc.status.address, addr->address, sizeof(g_poc.status.address));
    g_poc.status.address_valid = 1;
    g_poc.status.filter_used = filter;
    mutexUnlock(&g_poc.mutex);
}

static void pocRecordB0Write(Result rc)
{
    mutexLock(&g_poc.mutex);
    if (R_SUCCEEDED(rc)) {
        g_poc.status.b0_write_count++;
        g_poc.status.milestone |= DGLAB_POC_MILESTONE_B0_WRITTEN;
    } else {
        g_poc.status.b0_write_failures++;
    }
    mutexUnlock(&g_poc.mutex);
}

// ---------------------------------------------------------------------------
// Event helper
// ---------------------------------------------------------------------------

static bool pocAcquireEvent(Event* event, bool* active, const char* name,
    Result (*acquire)(Event*))
{
    Result rc;

    if (*active)
        return true;

    memset(event, 0, sizeof(*event));
    rc = acquire(event);
    pocLog("%s rc=0x%08X", name, (u32)rc);

    if (R_FAILED(rc))
        return false;

    *active = true;
    return true;
}

// ---------------------------------------------------------------------------
// Outgoing B0 traffic
// ---------------------------------------------------------------------------

// Forward declarations: actions are handled from inside the scan poll loop too.
static bool pocTakeAction(PocWorker* w, u32* out_action);
static bool pocHandleAction(PocWorker* w, u32 action);

static Result pocWriteCharacteristic(BtdevGattCharacteristic* characteristic, const u8* data,
    size_t size)
{
    Result rc;

    btdevGattCharacteristicSetValue(characteristic, data, size);
    rc = btdevWriteGattCharacteristic(characteristic);

    return rc;
}

// A B0 packet that changes nothing: no strength change, both channels idle. The
// device discards invalid channel data, so this exercises the write path without
// producing output.
static void pocWriteIdleB0(PocWorker* w)
{
    DglabCoyoteV3B0 b0;
    u8 packet[DGLAB_COYOTE_V3_B0_SIZE];
    Result rc;
    u32 written;

    memset(&b0, 0, sizeof(b0));
    dglabCoyoteV3EncodeB0(&b0, packet);

    rc = pocWriteCharacteristic(&w->char_write, packet, sizeof(packet));
    pocRecordB0Write(rc);

    mutexLock(&g_poc.mutex);
    written = g_poc.status.b0_write_count + g_poc.status.b0_write_failures;
    mutexUnlock(&g_poc.mutex);

    if (written == 1 || written % 100 == 0)
        pocLog("b0 idle write #%u rc=0x%08X", written, (u32)rc);
    else if (R_FAILED(rc))
        pocLog("b0 idle write failed rc=0x%08X", (u32)rc);
}

// Sets both channels to strength 0 with a non-zero sequence number, so the
// device must answer with B1 carrying the same sequence number.
static void pocWriteZeroB0(PocWorker* w)
{
    DglabCoyoteV3B0 b0;
    u8 packet[DGLAB_COYOTE_V3_B0_SIZE];
    Result rc;

    memset(&b0, 0, sizeof(b0));
    b0.sequence = 1;
    b0.strength_a.mode = DglabCoyoteV3StrengthMode_Absolute;
    b0.strength_a.value = 0;
    b0.strength_b.mode = DglabCoyoteV3StrengthMode_Absolute;
    b0.strength_b.value = 0;
    dglabCoyoteV3EncodeB0(&b0, packet);

    pocLog("b0 zero write, sequence 1, expecting B1");
    rc = pocWriteCharacteristic(&w->char_write, packet, sizeof(packet));
    pocRecordB0Write(rc);
    pocLog("b0 zero write rc=0x%08X", (u32)rc);
}

static void pocReadBattery(PocWorker* w)
{
    Result rc;

    if (!w->have_battery) {
        pocLog("battery read skipped: characteristic not resolved");
        return;
    }

    rc = btdevReadGattCharacteristic(&w->char_battery);
    pocLog("battery read rc=0x%08X", (u32)rc);
}

// ---------------------------------------------------------------------------
// GATT operation results (notifications, read responses)
// ---------------------------------------------------------------------------

static void pocHandleGattOperation(PocWorker* w, const BtdrvBleClientGattOperationInfo* op)
{
    u32 size = (u32)op->size;
    char hex[3 * 20 + 1];

    if (size > sizeof(op->data))
        size = sizeof(op->data);

    if (size > 20)
        size = 20;

    mutexLock(&g_poc.mutex);
    g_poc.status.notify_count++;
    g_poc.status.last_notify_size = size;

    if (size)
        memcpy(g_poc.status.last_notify, op->data, size);

    mutexUnlock(&g_poc.mutex);

    if (w->notify_logged < POC_NOTIFY_LOG_LIMIT) {
        pocHex(hex, sizeof(hex), op->data, size);
        pocLog("gatt op size=%u data=%s", size, hex);
        w->notify_logged++;
    }

    if (size >= DGLAB_COYOTE_V3_B1_SIZE && op->data[0] == DGLAB_COYOTE_V3_HEADER_B1) {
        DglabCoyoteV3B1 b1;

        if (dglabCoyoteV3DecodeB1(op->data, size, &b1)) {
            pocSetMilestone(DGLAB_POC_MILESTONE_B1_RECEIVED);
            pocLog("B1 sequence=%u A=%u B=%u", b1.sequence, b1.strength_a, b1.strength_b);
        }
    } else if (size == 1 && w->have_battery) {
        mutexLock(&g_poc.mutex);
        g_poc.status.battery_value = op->data[0];
        g_poc.status.battery_valid = 1;
        g_poc.status.milestone |= DGLAB_POC_MILESTONE_BATTERY_READ;
        mutexUnlock(&g_poc.mutex);
        pocLog("battery value=%u", op->data[0]);
    }
}

static void pocDrainGattOperations(PocWorker* w)
{
    for (u32 i = 0; i < 8; i++) {
        BtdrvBleClientGattOperationInfo op;
        Result rc;

        memset(&op, 0, sizeof(op));
        rc = btdevGetGattOperationResult(&op);

        if (R_FAILED(rc)) {
            if (i == 0 && w->gatt_polls % 50 == 0)
                pocLog("getGattOperationResult rc=0x%08X", (u32)rc);
            break;
        }

        // An empty queue reports success with a zeroed payload, so only a
        // payload with actual data is treated as an operation result.
        if (op.size == 0)
            break;

        pocHandleGattOperation(w, &op);
    }

    w->gatt_polls++;
}

// ---------------------------------------------------------------------------
// Session steps
// ---------------------------------------------------------------------------

static void pocStopScan(PocWorker* w)
{
    if (!w->scanning)
        return;

    Result rc = btdevStopBleScanSmartDevice();
    pocLog("btdevStopBleScanSmartDevice rc=0x%08X", (u32)rc);
    w->scanning = false;
}

static bool pocScan_eventSetup(PocWorker* w)
{
    return pocAcquireEvent(&w->scan_event, &w->scan_event_active, "btdevAcquireBleScanEvent",
        btdevAcquireBleScanEvent);
}

// btm keeps its scan filters in system state that a third party cannot set, and
// the scan start functions take a filter from the caller. Logging the stored
// values shows whether our filter can ever match anything.
static void pocLogStoredScanParameters(void)
{
    BtdrvBleAdvertisePacketParameter param;
    BtdrvGattAttributeUuid uuid;
    Result rc;

    memset(&param, 0, sizeof(param));
    rc = btdevGetBleScanParameter(0xFFFFu, &param);
    pocLog("stored scan param 0xFFFF rc=0x%08X company=0x%04X pattern=%02X%02X%02X%02X%02X%02X",
        (u32)rc, param.company_id, param.pattern_data[0], param.pattern_data[1],
        param.pattern_data[2], param.pattern_data[3], param.pattern_data[4],
        param.pattern_data[5]);

    memset(&param, 0, sizeof(param));
    rc = btdevGetBleScanParameter(0x0001u, &param);
    pocLog("stored scan param 0x0001 rc=0x%08X company=0x%04X pattern=%02X%02X%02X%02X%02X%02X",
        (u32)rc, param.company_id, param.pattern_data[0], param.pattern_data[1],
        param.pattern_data[2], param.pattern_data[3], param.pattern_data[4],
        param.pattern_data[5]);

    memset(&uuid, 0, sizeof(uuid));
    rc = btdevGetBleScanParameter2(0x0002u, &uuid);
    pocLog("stored smart device UUID rc=0x%08X size=0x%X bytes=%02X%02X%02X%02X", (u32)rc,
        uuid.size, uuid.uuid[0], uuid.uuid[1], uuid.uuid[2], uuid.uuid[3]);
}

// Waits for scan results and returns the first device seen.
static bool pocPollScanResults(PocWorker* w, const char* label, BtdrvAddress* out)
{
    u32 deadline = pocNowMs() + POC_SCAN_TIMEOUT_MS;

    while (pocNowMs() < deadline) {
        BtdrvBleScanResult results[10];
        u8 total = 0;
        Result rc;
        u32 action;

        if (pocStopRequested() || w->restart_scan)
            return false;

        // Actions must be honoured while a scan is running: the user presses a
        // key when nothing is happening, which is exactly when a scan is in
        // progress. Dropping them here made the driver level probe look dead.
        while (pocTakeAction(w, &action)) {
            w->scan_attempts = 0;

            if (pocHandleAction(w, action))
                return false;
        }

        Result wait_rc = eventWait(&w->scan_event, 500ull * 1000000ull);

        if (R_SUCCEEDED(wait_rc)) {
            // The scan event firing at all is a separate signal from getting
            // results: it says btm is running the scan.
            w->scan_events++;

            if (w->scan_events <= 5 || w->scan_events % 20 == 0)
                pocLog("%s scan event #%u", label, w->scan_events);
        }

        memset(results, 0, sizeof(results));
        rc = btdevGetBleScanResult(results, 10, &total);

        w->scan_polls++;

        if (R_FAILED(rc) || total == 0) {
            if (w->scan_polls % POC_SCAN_POLL_LOG_EVERY == 0)
                pocLog("%s scan poll %u rc=0x%08X total=%u", label, w->scan_polls, (u32)rc,
                    total);
            continue;
        }

        pocRecordScanResult();
        pocLog("%s scan found %u device(s)", label, total);

        for (u8 i = 0; i < total && i < 10; i++) {
            pocLog("  %s scan %u addr=%02X:%02X:%02X:%02X:%02X:%02X", label, i,
                results[i].addr.address[0], results[i].addr.address[1],
                results[i].addr.address[2], results[i].addr.address[3],
                results[i].addr.address[4], results[i].addr.address[5]);
        }

        *out = results[0].addr;
        return true;
    }

    pocLog("%s scan timed out", label);
    pocLog("%s scan summary: events=%u polls=%u devices=0", label, w->scan_events, w->scan_polls);
    return false;
}

// Scans filtered by one service UUID. 0x1812 is what the device advertises.
static bool pocScanSmart(PocWorker* w, u16 filter_uuid, BtdrvAddress* out)
{
    BtdrvGattAttributeUuid filter = pocUuid16(filter_uuid);
    Result rc;

    if (!pocScan_eventSetup(w))
        return false;

    rc = btdevStartBleScanSmartDevice(&filter);
    pocLog("btdevStartBleScanSmartDevice(0x%04X) rc=0x%08X", filter_uuid, (u32)rc);

    if (R_FAILED(rc))
        return false;

    w->scanning = true;
    w->scan_polls = 0;
    pocSetMilestone(DGLAB_POC_MILESTONE_SCAN_STARTED);
    pocSetState(DglabPocState_Scanning);

    char label[32];
    snprintf(label, sizeof(label), "0x%04X", filter_uuid);

    bool found = pocPollScanResults(w, label, out);

    pocStopScan(w);
    return found;
}

// Control experiment: btm's general scan uses a manufacturer data filter.
static bool pocScanGeneral(PocWorker* w, BtdrvAddress* out)
{
    BtdrvBleAdvertisePacketParameter param;
    Result rc;

    if (!pocScan_eventSetup(w))
        return false;

    memset(&param, 0, sizeof(param));
    Result param_rc = btdevGetBleScanParameter(0xFFFFu, &param);
    pocLog("btdevGetBleScanParameter(0xFFFF) rc=0x%08X company=0x%04X pattern=%02X%02X%02X%02X%02X%02X",
        (u32)param_rc, param.company_id, param.pattern_data[0], param.pattern_data[1],
        param.pattern_data[2], param.pattern_data[3], param.pattern_data[4],
        param.pattern_data[5]);

    rc = btdevStartBleScanGeneral(param);
    pocLog("btdevStartBleScanGeneral rc=0x%08X", (u32)rc);

    if (R_FAILED(rc))
        return false;

    w->scan_polls = 0;
    pocSetMilestone(DGLAB_POC_MILESTONE_SCAN_STARTED);
    pocSetState(DglabPocState_Scanning);

    bool found = pocPollScanResults(w, "general", out);

    rc = btdevStopBleScanGeneral();
    pocLog("btdevStopBleScanGeneral rc=0x%08X", (u32)rc);

    return found;
}

static bool pocScanAny(PocWorker* w, BtdrvAddress* out)
{
    if (w->forced_filter == POC_UUID16_ADVERTISED_SERVICE)
        return pocScanSmart(w, POC_UUID16_ADVERTISED_SERVICE, out);

    if (w->forced_filter == DGLAB_COYOTE_V3_UUID16_SERVICE)
        return pocScanSmart(w, DGLAB_COYOTE_V3_UUID16_SERVICE, out);

    if (w->forced_filter == 0xFFFFu)
        return pocScanGeneral(w, out);

    // Default order: the advertised UUID first (the DG-LAB service only exists
    // after connecting and cannot be used as a scan filter), then the protocol
    // UUID, then btm's general scan as the last control attempt.
    if (pocScanSmart(w, POC_UUID16_ADVERTISED_SERVICE, out)) {
        w->filter_used = POC_UUID16_ADVERTISED_SERVICE;
        return true;
    }

    if (pocStopRequested() || w->restart_scan)
        return false;

    pocLog("falling back to the protocol service UUID 0x180C for scanning");

    if (pocScanSmart(w, DGLAB_COYOTE_V3_UUID16_SERVICE, out)) {
        w->filter_used = DGLAB_COYOTE_V3_UUID16_SERVICE;
        return true;
    }

    if (pocStopRequested() || w->restart_scan)
        return false;

    pocLog("falling back to the general (manufacturer) scan filter");

    if (pocScanGeneral(w, out)) {
        w->filter_used = 0xFFFFu;
        return true;
    }

    return false;
}

static bool pocConnect(PocWorker* w)
{
    Result rc;
    u32 deadline;

    if (!pocAcquireEvent(&w->conn_event, &w->conn_event_active,
            "btdevAcquireBleConnectionStateChangedEvent",
            btdevAcquireBleConnectionStateChangedEvent))
        return false;

    rc = btdevConnectToGattServer(w->address);
    pocLog("btdevConnectToGattServer rc=0x%08X", (u32)rc);

    if (R_FAILED(rc))
        return false;

    pocSetState(DglabPocState_Connecting);
    deadline = pocNowMs() + POC_CONNECT_TIMEOUT_MS;

    while (pocNowMs() < deadline) {
        BtdrvBleConnectionInfo info[4];
        u8 total = 0;

        if (pocStopRequested())
            return false;

        eventWait(&w->conn_event, 500ull * 1000000ull);

        memset(info, 0, sizeof(info));
        rc = btdevGetBleConnectionInfoList(info, 4, &total);

        if (R_FAILED(rc))
            continue;

        for (u8 i = 0; i < total && i < 4; i++) {
            pocLog("conn %u handle=%u addr=%02X:%02X:%02X:%02X:%02X:%02X", i,
                info[i].connection_handle, info[i].addr.address[0], info[i].addr.address[1],
                info[i].addr.address[2], info[i].addr.address[3], info[i].addr.address[4],
                info[i].addr.address[5]);

            if (memcmp(info[i].addr.address, w->address.address, 6) != 0)
                continue;

            w->connection_handle = info[i].connection_handle;
            w->connected = true;

            mutexLock(&g_poc.mutex);
            g_poc.status.conn_id = w->connection_handle;
            mutexUnlock(&g_poc.mutex);

            pocSetMilestone(DGLAB_POC_MILESTONE_CONNECTED);
            return true;
        }
    }

    pocLog("connect timed out");
    return false;
}

static void pocDisconnect(PocWorker* w)
{
    if (!w->connected)
        return;

    Result rc = btdevDisconnectFromGattServer(w->connection_handle);
    pocLog("btdevDisconnectFromGattServer rc=0x%08X", (u32)rc);
    w->connected = false;
}

static bool pocDiscover(PocWorker* w)
{
    BtdrvGattAttributeUuid service_uuid = pocUuid16(DGLAB_COYOTE_V3_UUID16_SERVICE);
    BtdrvGattAttributeUuid write_uuid = pocUuid16(DGLAB_COYOTE_V3_UUID16_CHAR_WRITE);
    BtdrvGattAttributeUuid notify_uuid = pocUuid16(DGLAB_COYOTE_V3_UUID16_CHAR_NOTIFY);
    bool flag = false;
    Result rc = 0;

    pocSetState(DglabPocState_Discovering);

    if (!pocAcquireEvent(&w->discovery_event, &w->discovery_event_active,
            "btdevAcquireBleServiceDiscoveryEvent", btdevAcquireBleServiceDiscoveryEvent))
        return false;

    u32 deadline = pocNowMs() + POC_DISCOVER_TIMEOUT_MS;

    while (pocNowMs() < deadline && !flag) {
        if (pocStopRequested())
            return false;

        eventWait(&w->discovery_event, 500ull * 1000000ull);

        rc = btdevGetGattService(w->connection_handle, &service_uuid, &w->service, &flag);
        pocLog("btdevGetGattService(0x180C) rc=0x%08X flag=%u", (u32)rc, flag);
    }

    if (!flag) {
        pocFail(rc, "service 0x180C");
        return false;
    }

    pocSetMilestone(DGLAB_POC_MILESTONE_SERVICE_FOUND);

    bool write_flag = false;
    bool notify_flag = false;

    rc = btdevGattServiceGetCharacteristic(&w->service, &write_uuid, &w->char_write, &write_flag);
    pocLog("char 0x150A rc=0x%08X flag=%u prop=0x%02X", (u32)rc, write_flag,
        btdevGattCharacteristicGetProperties(&w->char_write));

    Result notify_rc = btdevGattServiceGetCharacteristic(&w->service, &notify_uuid,
        &w->char_notify, &notify_flag);
    pocLog("char 0x150B rc=0x%08X flag=%u prop=0x%02X", (u32)notify_rc, notify_flag,
        btdevGattCharacteristicGetProperties(&w->char_notify));

    if (!write_flag || !notify_flag) {
        pocFail(R_FAILED(rc) ? rc : notify_rc, "characteristics");
        return false;
    }

    w->have_write = true;
    w->have_notify = true;
    pocSetCharProperty(&g_poc.status.char_write_prop,
        btdevGattCharacteristicGetProperties(&w->char_write));
    pocSetCharProperty(&g_poc.status.char_notify_prop,
        btdevGattCharacteristicGetProperties(&w->char_notify));
    pocSetMilestone(DGLAB_POC_MILESTONE_CHARS_FOUND);

    // The battery characteristic is optional: it is only needed for the read test.
    BtdrvGattAttributeUuid battery_service_uuid = pocUuid16(DGLAB_COYOTE_V3_UUID16_BATTERY_SERVICE);
    bool battery_service_flag = false;

    rc = btdevGetGattService(w->connection_handle, &battery_service_uuid, &w->battery_service,
        &battery_service_flag);
    pocLog("btdevGetGattService(0x180A) rc=0x%08X flag=%u", (u32)rc, battery_service_flag);

    if (battery_service_flag) {
        BtdrvGattAttributeUuid battery_uuid = pocUuid16(DGLAB_COYOTE_V3_UUID16_CHAR_BATTERY);
        bool battery_flag = false;

        rc = btdevGattServiceGetCharacteristic(&w->battery_service, &battery_uuid,
            &w->char_battery, &battery_flag);
        pocLog("char 0x1500 rc=0x%08X flag=%u prop=0x%02X", (u32)rc, battery_flag,
            btdevGattCharacteristicGetProperties(&w->char_battery));

        w->have_battery = battery_flag;
        pocSetCharProperty(&g_poc.status.char_battery_prop,
            btdevGattCharacteristicGetProperties(&w->char_battery));
    }

    return true;
}

static bool pocSubscribe(PocWorker* w)
{
    Result rc = btdevEnableGattCharacteristicNotification(&w->char_notify, true);
    pocLog("btdevEnableGattCharacteristicNotification rc=0x%08X", (u32)rc);

    if (R_FAILED(rc)) {
        pocFail(rc, "enable notification");
        return false;
    }

    if (!pocAcquireEvent(&w->gatt_event, &w->gatt_event_active,
            "btdevAcquireBleGattOperationEvent", btdevAcquireBleGattOperationEvent))
        return false;

    pocSetMilestone(DGLAB_POC_MILESTONE_NOTIFY_ON);
    pocSetState(DglabPocState_Ready);
    return true;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// Last driver-level attempt.
//
// btm sets the BLE scan interval/window before it scans, so a zero default could
// be the reason btdrv's own scan produced nothing in the earlier runs. This probe
// sets explicit scan parameters, tries an unfiltered scan and a scan filtered on
// the advertised service UUID, and polls the event queue directly instead of
// relying on the event handle firing.
static void pocRunBtdrvScanProbe(PocWorker* w)
{
    static const u16 kInterval[2] = { 0x0060u, 0x0030u };
    static const u16 kWindow[2] = { 0x0030u, 0x0030u };
    Event event;
    Result rc;
    u32 total_scan_results = 0;

    pocStopScan(w);

    rc = btdrvInitialize();
    pocLog("btdrv probe: btdrvInitialize rc=0x%08X", (u32)rc);

    if (R_FAILED(rc))
        return;

    memset(&event, 0, sizeof(event));
    rc = btdrvInitializeBle(&event);
    pocLog("btdrv probe: btdrvInitializeBle rc=0x%08X", (u32)rc);

    if (R_FAILED(rc)) {
        btdrvExit();
        return;
    }

    bool enabled = false;
    btdrvIsBluetoothEnabled(&enabled);
    pocLog("btdrv probe: adapter enabled=%u", enabled ? 1u : 0u);

    rc = btdrvEnableBle();
    pocLog("btdrv probe: btdrvEnableBle rc=0x%08X", (u32)rc);

    for (u32 phase = 0; phase < 2 && !pocStopRequested(); phase++) {
        u32 deadline;
        u32 fetches = 0;
        u32 scan_results = 0;

        rc = btdrvSetBleScanParameter(kInterval[phase], kWindow[phase]);
        pocLog("btdrv probe: SetBleScanParameter(0x%04X, 0x%04X) rc=0x%08X", kInterval[phase],
            kWindow[phase], (u32)rc);

        if (phase == 1) {
            BtdrvBleAdvertiseFilter filter;

            memset(&filter, 0, sizeof(filter));
            filter.index = 0;
            filter.adv.size = 2;
            filter.adv.type = 0x03; // complete list of 16-bit service UUIDs
            filter.adv.data[0] = 0x12;
            filter.adv.data[1] = 0x18; // 0x1812, little endian
            filter.mask[0] = 0xFF;
            filter.mask[1] = 0xFF;
            filter.mask_size = 2;

            rc = btdrvAddBleScanFilterCondition(&filter);
            pocLog("btdrv probe: AddBleScanFilterCondition(0x1812) rc=0x%08X", (u32)rc);

            rc = btdrvEnableBleScanFilter(true);
            pocLog("btdrv probe: EnableBleScanFilter(true) rc=0x%08X", (u32)rc);
        }

        rc = btdrvStartBleScan();
        pocLog("btdrv probe: btdrvStartBleScan (phase %u) rc=0x%08X", phase, (u32)rc);

        deadline = pocNowMs() + 10000u;

        while (pocNowMs() < deadline && !pocStopRequested()) {
            BtdrvBleEventInfo info;
            BtdrvBleEventType type = 0;

            eventWait(&event, 200ull * 1000000ull);

            memset(&info, 0, sizeof(info));
            rc = btdrvGetBleManagedEventInfo(&info, sizeof(info), &type);
            fetches++;

            if (R_FAILED(rc)) {
                if (fetches % 25 == 0)
                    pocLog("btdrv probe: get event info rc=0x%08X", (u32)rc);
                continue;
            }

            if (fetches <= 3) {
                pocLog("btdrv probe: fetch type=%u raw=%02X%02X%02X%02X%02X%02X%02X%02X",
                    (u32)type, info.data[0], info.data[1], info.data[2], info.data[3],
                    info.data[4], info.data[5], info.data[6], info.data[7]);
            }

            if (type != BtdrvBleEventType_ScanResult)
                continue;

            scan_results++;
            total_scan_results++;

            if (scan_results <= 5) {
                pocLog("btdrv probe: scan result status=%u addr=%02X:%02X:%02X:%02X:%02X:%02X entries=%u rssi=%d",
                    info.scan_result.status, info.scan_result.address.address[0],
                    info.scan_result.address.address[1], info.scan_result.address.address[2],
                    info.scan_result.address.address[3], info.scan_result.address.address[4],
                    info.scan_result.address.address[5], info.scan_result.count,
                    info.scan_result.rssi);
            }
        }

        pocLog("btdrv probe: phase %u done fetches=%u scan_results=%u", phase, fetches,
            scan_results);

        btdrvStopBleScan();
    }

    btdrvClearBleScanFilters();
    pocLog("btdrv probe: done, %u scan result(s) in total", total_scan_results);

    eventClose(&event);
    btdrvExit();
}

// Returns true when the session should go back to scanning.
static bool pocHandleAction(PocWorker* w, u32 action)
{
    switch (action) {
        case DglabPocAction_WriteIdleB0:
            if (w->connected && w->have_write)
                pocWriteIdleB0(w);
            break;

        case DglabPocAction_WriteZeroB0:
            if (w->connected && w->have_write)
                pocWriteZeroB0(w);
            break;

        case DglabPocAction_ReadBattery:
            if (w->connected && w->have_battery)
                pocReadBattery(w);
            break;

        case DglabPocAction_ToggleAutoWrite: {
            mutexLock(&g_poc.mutex);
            g_poc.auto_write = !g_poc.auto_write;
            u32 enabled = g_poc.auto_write ? 1u : 0u;
            g_poc.status.auto_write = enabled;
            mutexUnlock(&g_poc.mutex);
            pocLog("auto_write=%u", enabled);
            break;
        }

        case DglabPocAction_ScanWithAdvertisedUuid:
            pocLog("action: scan with the advertised UUID 0x1812");
            w->forced_filter = POC_UUID16_ADVERTISED_SERVICE;
            w->restart_scan = true;
            return true;

        case DglabPocAction_ScanWithProtocolUuid:
            pocLog("action: scan with the protocol UUID 0x180C");
            w->forced_filter = DGLAB_COYOTE_V3_UUID16_SERVICE;
            w->restart_scan = true;
            return true;

        case DglabPocAction_ScanWithGeneralFilter:
            pocLog("action: scan with the general (manufacturer) filter");
            w->forced_filter = 0xFFFFu;
            w->restart_scan = true;
            return true;

        case DglabPocAction_Rescan:
        case DglabPocAction_RestartSession:
            pocLog("action: rescan");
            w->forced_filter = 0;
            w->restart_scan = true;
            return true;

        case DglabPocAction_ProbeBtdrvScan:
            // Deferred: run it from the session loop, never from inside a scan
            // poll, so the scan bookkeeping stays consistent.
            pocLog("action: btdrv scan probe");
            w->probe_btdrv = true;
            w->restart_scan = true;
            return true;

        case DglabPocAction_Disconnect:
            pocLog("action: disconnect");
            w->restart_scan = true;
            return true;

        default:
            break;
    }

    return false;
}

static bool pocTakeAction(PocWorker* w, u32* out_action)
{
    mutexLock(&g_poc.mutex);
    u32 action = g_poc.pending_action;
    g_poc.pending_action = 0;
    mutexUnlock(&g_poc.mutex);

    if (action == 0)
        return false;

    *out_action = action;
    return true;
}

// ---------------------------------------------------------------------------
// Connected loop
// ---------------------------------------------------------------------------

static void pocConnectedLoop(PocWorker* w)
{
    w->next_b0_ms = pocNowMs();

    while (!pocStopRequested() && !w->restart_scan) {
        u32 action;
        bool auto_write;

        if (w->gatt_event_active) {
            eventWait(&w->gatt_event, 10000000ull); // 10ms
            pocDrainGattOperations(w);
        } else {
            svcSleepThread(10000000ull);
        }

        while (pocTakeAction(w, &action)) {
            if (pocHandleAction(w, action))
                return;
        }

        mutexLock(&g_poc.mutex);
        auto_write = g_poc.auto_write;
        mutexUnlock(&g_poc.mutex);

        if (auto_write && w->have_write && (s32)(pocNowMs() - w->next_b0_ms) >= 0) {
            w->next_b0_ms = pocNowMs() + POC_B0_INTERVAL_MS;
            pocWriteIdleB0(w);
        }
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void pocCleanup(PocWorker* w)
{
    pocStopScan(w);
    pocDisconnect(w);

    if (w->scan_event_active) {
        eventClose(&w->scan_event);
        w->scan_event_active = false;
    }

    if (w->conn_event_active) {
        eventClose(&w->conn_event);
        w->conn_event_active = false;
    }

    if (w->discovery_event_active) {
        eventClose(&w->discovery_event);
        w->discovery_event_active = false;
    }

    if (w->gatt_event_active) {
        eventClose(&w->gatt_event);
        w->gatt_event_active = false;
    }

    if (w->ble_ready) {
        btdevExit();
        w->ble_ready = false;
    }
}

static void pocThreadFunc(void* arg)
{
    PocWorker* w = &g_poc.worker;
    Result rc;

    (void)arg;

    memset(w, 0, sizeof(*w));

    pocSetState(DglabPocState_Initializing);
    pocLog("poc start aruid_low=0x%08X", (u32)g_poc.aruid);

    rc = btdevInitialize();
    pocLog("btdevInitialize rc=0x%08X", (u32)rc);

    if (R_FAILED(rc)) {
        pocFail(rc, "btdevInitialize");
        goto out;
    }

    w->ble_ready = true;
    pocSetMilestone(DGLAB_POC_MILESTONE_BLE_READY);

    pocLogStoredScanParameters();

    if (g_poc.use_target_address) {
        pocLog("direct connect to %02X:%02X:%02X:%02X:%02X:%02X, scan skipped",
            g_poc.target_address[0], g_poc.target_address[1], g_poc.target_address[2],
            g_poc.target_address[3], g_poc.target_address[4], g_poc.target_address[5]);
    } else if (g_poc.start_scan_filter != 0) {
        pocLog("scan filter forced to 0x%04X", g_poc.start_scan_filter);
        w->forced_filter = (u16)g_poc.start_scan_filter;
    }

    // The driver level scan is the last untested path and it must not depend on
    // the user pressing a key while the right step happens to be running, so it
    // runs once automatically at the start of every session. The Left key can
    // still repeat it on demand.
    pocRunBtdrvScanProbe(w);

    while (!pocStopRequested()) {
        BtdrvAddress address;
        u32 action;

        while (pocTakeAction(w, &action)) {
            if (pocHandleAction(w, action))
                break;
        }

        if (w->probe_btdrv) {
            w->probe_btdrv = false;
            pocRunBtdrvScanProbe(w);
            continue;
        }

        w->restart_scan = false;

        if (g_poc.use_target_address) {
            memcpy(address.address, g_poc.target_address, sizeof(address.address));
            w->filter_used = 0;
            pocSetMilestone(DGLAB_POC_MILESTONE_DEVICE_FOUND);
        } else if (!pocScanAny(w, &address)) {
            if (pocStopRequested() || w->restart_scan)
                continue;

            w->scan_attempts++;

            if (w->scan_attempts >= POC_SCAN_ATTEMPTS) {
                pocFail(MAKERESULT(Module_Libnx, LibnxError_Timeout), "scan");
                break;
            }

            continue;
        }

        pocRecordMatch(&address, w->filter_used);

        if (!pocConnect(w)) {
            if (pocStopRequested() || w->restart_scan)
                continue;

            w->scan_attempts++;

            if (w->scan_attempts >= POC_SCAN_ATTEMPTS) {
                pocFail(MAKERESULT(Module_Libnx, LibnxError_Timeout), "connect");
                break;
            }

            continue;
        }

        if (!pocDiscover(w) || !pocSubscribe(w)) {
            pocDisconnect(w);
            break;
        }

        pocConnectedLoop(w);
        pocDisconnect(w);
    }

out:
    pocCleanup(w);

    mutexLock(&g_poc.mutex);
    bool failed = g_poc.status.state == DglabPocState_Failed;

    if (!failed && g_poc.status.state != DglabPocState_Ready)
        g_poc.status.state = DglabPocState_Stopped;

    g_poc.running = false;
    g_poc.stop_requested = false;
    mutexUnlock(&g_poc.mutex);

    pocLog("poc end state=%s result=0x%08X", failed ? "failed" : "stopped",
        g_poc.status.last_result);
}

// ---------------------------------------------------------------------------
// IPC entry points
// ---------------------------------------------------------------------------

void blePocInitialize(void)
{
    memset(&g_poc, 0, sizeof(g_poc));
    mutexInit(&g_poc.mutex);
    g_poc.auto_write = true;
    g_poc.status.state = DglabPocState_Idle;
    g_poc.status.auto_write = 1;
}

Result blePocStart(const DglabPocStartRequest* request)
{
    Result rc;

    mutexLock(&g_poc.mutex);

    if (g_poc.running) {
        mutexUnlock(&g_poc.mutex);
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    Thread previous = g_poc.worker_thread;

    mutexUnlock(&g_poc.mutex);

    if (previous.handle != INVALID_HANDLE) {
        threadWaitForExit(&previous);
        threadClose(&previous);
    }

    mutexLock(&g_poc.mutex);
    memset(&g_poc.worker_thread, 0, sizeof(g_poc.worker_thread));
    memset(&g_poc.status, 0, sizeof(g_poc.status));

    // Clear the text but keep log_write_offset monotonic: readers track absolute
    // offsets, and resetting it made them read stale bytes from the previous run.
    memset(g_poc.log, 0, sizeof(g_poc.log));
    g_poc.log_valid_from = g_poc.log_write_offset;

    g_poc.status.state = DglabPocState_Initializing;
    g_poc.status.auto_write = g_poc.auto_write ? 1u : 0u;
    g_poc.pending_action = 0;
    g_poc.stop_requested = false;
    g_poc.aruid = request->applet_resource_user_id;
    g_poc.status.aruid_low = (u32)g_poc.aruid;
    g_poc.use_target_address = (request->flags & DGLAB_POC_START_FLAG_TARGET_ADDRESS) != 0;
    memcpy(g_poc.target_address, request->target_address, sizeof(g_poc.target_address));
    g_poc.start_scan_filter = request->scan_filter;
    g_poc.running = true;
    mutexUnlock(&g_poc.mutex);

    rc = threadCreate(&g_poc.worker_thread, pocThreadFunc, NULL, g_poc_thread_stack,
        sizeof(g_poc_thread_stack), 0x2C, 3);

    if (R_FAILED(rc)) {
        mutexLock(&g_poc.mutex);
        g_poc.running = false;
        g_poc.status.state = DglabPocState_Failed;
        g_poc.status.last_result = (u32)rc;
        mutexUnlock(&g_poc.mutex);
        return rc;
    }

    rc = threadStart(&g_poc.worker_thread);

    if (R_FAILED(rc)) {
        threadClose(&g_poc.worker_thread);
        memset(&g_poc.worker_thread, 0, sizeof(g_poc.worker_thread));

        mutexLock(&g_poc.mutex);
        g_poc.running = false;
        g_poc.status.state = DglabPocState_Failed;
        g_poc.status.last_result = (u32)rc;
        mutexUnlock(&g_poc.mutex);
        return rc;
    }

    return 0;
}

Result blePocStop(void)
{
    mutexLock(&g_poc.mutex);
    g_poc.stop_requested = true;
    mutexUnlock(&g_poc.mutex);

    return 0;
}

Result blePocAction(const DglabPocActionRequest* request)
{
    mutexLock(&g_poc.mutex);
    bool running = g_poc.running;
    g_poc.pending_action = request->action;
    mutexUnlock(&g_poc.mutex);

    if (!running) {
        pocLog("action %u ignored: no run active", request->action);
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }

    pocLog("action queued %u", request->action);

    return 0;
}

void blePocGetStatus(DglabPocStatus* out)
{
    mutexLock(&g_poc.mutex);
    *out = g_poc.status;
    mutexUnlock(&g_poc.mutex);
}

u32 blePocReadLog(u32 cursor, char* out, u32 out_size)
{
    u32 write;
    u32 earliest;
    u32 count;

    if (out_size == 0)
        return cursor;

    mutexLock(&g_poc.mutex);

    write = g_poc.log_write_offset;
    earliest = (write > POC_LOG_CAPACITY) ? write - POC_LOG_CAPACITY : 0;

    // Never hand out bytes from before the current run: the ring is cleared at
    // start, and starting mid line also produced a truncated first line.
    if (g_poc.log_valid_from > earliest)
        earliest = g_poc.log_valid_from;

    // A reader that is ahead of the writer (for example after the ring was
    // cleared) simply starts from the write position.
    if (cursor > write)
        cursor = write;

    if (cursor < earliest)
        cursor = earliest;

    count = write - cursor;
    if (count > out_size - 1)
        count = out_size - 1;

    for (u32 i = 0; i < count; i++)
        out[i] = g_poc.log[(cursor + i) % POC_LOG_CAPACITY];

    mutexUnlock(&g_poc.mutex);

    out[count] = '\0';

    return cursor + count;
}
