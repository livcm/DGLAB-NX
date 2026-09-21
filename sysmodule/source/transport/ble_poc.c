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
// Large enough that one probe run fits: the drains log an event as two lines and
// a single drain used to be longer than the whole ring, so its output was
// overwritten before the NRO's reader polled it (2026-09-21 hardware round).
#define POC_LOG_CAPACITY 16384u
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

// Sentinel for the control scan in pocScanControlCompany: not a UUID, so it can
// never collide with a real filter.
#define POC_FILTER_CONTROL_COMPANY 0xFFFEu

// Note on a retracted reading of the firmware: the 0x40-byte GATT registration
// block this file used to send came from treating 0x159a28 as a command table,
// but that address is the btdrv service object's vtable. The firmware reads
// libnx's 0x14-byte BtdrvGattAttributeUuid, exactly as libnx sends it; see
// docs/ble-re.md, "判定（2026-09-21 夜，更正）".

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
    bool probe_identity;
    u32 control_scan_index; // Which company ID the control scan uses next.
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
    bool skip_probes;

    // Worker owned.
    PocWorker worker;
} PocShared;

static PocShared g_poc;

// The worker thread is created on demand, so its stack cannot live on the stack
// of whichever thread starts it.
static u8 g_poc_thread_stack[POC_THREAD_STACK_SIZE] __attribute__((aligned(0x1000)));

// The identity probe is the one measurement the static analysis is blocked on
// (docs/ble-re.md), and it has to happen before anything else touches BLE: the
// BLE manager remembers the session that initialized it, and after that session
// ends every BLE-side command answers 0xF601 (KernelError_ConnectionClosed).
// So it runs automatically on the first session after a boot - and only then,
// which leaves later sessions free of it for observing the scan path. Pressing
// the Right / StickR key still runs it on demand.
static bool g_identity_probe_pending = true;

// The managed BLE event payload is 0x400 bytes. It lives in .bss instead of on
// the worker's stack because tests/stack exists to fail new KB-scale frames
// (sysmodule/AGENTS.md, "线程与栈"); only the PoC worker thread touches it.
static BtdrvBleEventInfo g_ble_event;

// btm:u scan/connection results for the ARUID probe below. BtdrvBleScanResult is
// 0x148 bytes each, so two of them stay out of the worker's frame on purpose.
static BtdrvBleScanResult g_btmu_scan_results[2];
static BtdrvBleConnectionInfo g_btmu_connections[2];

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
static u32 pocDrainBleEvents(const char* label, u32 duration_ms, u8* out_client_if);

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

// Positive control for the scan path.
//
// btm's general scan is a manufacturer-data filter, and the value the system
// stores belongs to Nintendo (company 0x0553) - it matches nothing in a normal
// room. Asking for a company ID that phones and earbuds actually advertise is
// the only way to tell "btm never scans for this process" apart from "the filter
// matched nothing". Only a hit is a verdict; a miss is not (the pattern field
// may still be excluding everything).
static bool pocScanControlCompany(PocWorker* w, BtdrvAddress* out)
{
    static const u16 kCompanyIds[3] = { 0x004Cu, 0x0006u, 0x0075u }; // Apple, Microsoft, Samsung
    const u32 count = sizeof(kCompanyIds) / sizeof(kCompanyIds[0]);
    BtdrvBleAdvertisePacketParameter param;
    u16 company = kCompanyIds[w->control_scan_index % count];
    Result rc;

    w->control_scan_index++;

    if (!pocScan_eventSetup(w))
        return false;

    memset(&param, 0, sizeof(param));
    param.company_id = company;
    pocLog("control scan: company=0x%04X, pattern left zero", company);

    rc = btdevStartBleScanGeneral(param);
    pocLog("control scan: btdevStartBleScanGeneral rc=0x%08X", (u32)rc);

    if (R_FAILED(rc))
        return false;

    w->scan_polls = 0;
    pocSetMilestone(DGLAB_POC_MILESTONE_SCAN_STARTED);
    pocSetState(DglabPocState_Scanning);

    bool found = pocPollScanResults(w, "control", out);

    rc = btdevStopBleScanGeneral();
    pocLog("control scan: btdevStopBleScanGeneral rc=0x%08X", (u32)rc);

    return found;
}

static bool pocScanAny(PocWorker* w, BtdrvAddress* out)
{
    if (w->forced_filter == POC_UUID16_ADVERTISED_SERVICE)
        return pocScanSmart(w, POC_UUID16_ADVERTISED_SERVICE, out);

    if (w->forced_filter == DGLAB_COYOTE_V3_UUID16_SERVICE)
        return pocScanSmart(w, DGLAB_COYOTE_V3_UUID16_SERVICE, out);

    if (w->forced_filter == POC_FILTER_CONTROL_COMPANY)
        return pocScanControlCompany(w, out);

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
    static u8 previous_event[sizeof(((BtdrvBleEventInfo*)0)->data)];
    BtdrvAddress scanned_address;
    Event event;
    Result rc;
    bool have_previous = false;
    u8 client_if = 0xFF;
    bool have_address = false;
    u32 total_scan_results = 0;

    // Version marker: if a log has no line below this one, the build that ran
    // is older than the counters (2026-09-21 hardware round).
    pocLog("btdrv probe: v5 (dumps 0x60 bytes at the data start, spots repeats)");

    memset(&scanned_address, 0, sizeof(scanned_address));
    memset(previous_event, 0, sizeof(previous_event));

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

    // The manager registers its own GATT client when InitializeBle brings it
    // up; that event carries the interface number the connect call wants.
    pocDrainBleEvents("btdrv probe after InitializeBle", 1000u, &client_if);
    pocLog("btdrv probe: client_if=0x%02X", client_if);

    // Start from a known filter state: a filter left enabled by an earlier run
    // is one of the ways a scan can come back with nothing at all.
    rc = btdrvClearBleScanFilters();
    pocLog("btdrv probe: ClearBleScanFilters rc=0x%08X", (u32)rc);

    rc = btdrvEnableBleScanFilter(false);
    pocLog("btdrv probe: EnableBleScanFilter(false) rc=0x%08X", (u32)rc);

    for (u32 phase = 0; phase < 2 && !pocStopRequested(); phase++) {
        u32 deadline;
        u32 fetches = 0;
        u32 empties = 0;
        u32 events = 0;
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
            bool empty;
            u32 nonzero = 0;
            u32 first = sizeof(info.data);

            eventWait(&event, 200ull * 1000000ull);

            memset(&info, 0, sizeof(info));
            rc = btdrvGetBleManagedEventInfo(&info, sizeof(info), &type);
            fetches++;

            if (R_FAILED(rc)) {
                if (fetches % 25 == 0)
                    pocLog("btdrv probe: get event info rc=0x%08X", (u32)rc);
                continue;
            }

            // An idle queue answers rc=0 with type=0 and an all-zero payload, so
            // "non-empty" has to be decided from the bytes as well as the type
            // (docs/ble-poc.md, third hardware round). The 2026-09-21 run showed
            // type=0 with the first 16 bytes clear but bytes further in set, so
            // count the whole 0x400-byte answer and report where it starts.
            for (u32 i = 0; i < sizeof(info.data); i++) {
                if (info.data[i] != 0) {
                    if (first == sizeof(info.data))
                        first = i;
                    nonzero++;
                }
            }

            empty = (type == 0 && nonzero == 0);

            if (empty) {
                empties++;
                continue;
            }

            events++;

            // Three lines per event: where the data starts and what is there.
            if (events <= 3) {
                bool repeat = have_previous &&
                    memcmp(previous_event, info.data, sizeof(previous_event)) == 0;
                u32 base = (first & ~0xFu);

                if (base + 0x60u > sizeof(info.data))
                    base = sizeof(info.data) - 0x60u;

                pocLog("btdrv probe: event #%u type=%u nonzero=%u first=0x%03X repeat=%u",
                    events, (u32)type, nonzero, first, repeat ? 1u : 0u);

                for (u32 row = 0; row < 6; row++) {
                    const u8* p = info.data + base + row * 16u;

                    pocLog("btdrv probe:   %03X %02X%02X%02X%02X %02X%02X%02X%02X "
                        "%02X%02X%02X%02X %02X%02X%02X%02X", base + row * 16u,
                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                        p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
                }

                // The configured address is the one thing that tells a real scan
                // result from the manager's own noise: the device is advertising
                // while the probe runs.
                if (g_poc.use_target_address) {
                    for (u32 i = 0; i + 6u <= sizeof(info.data); i++) {
                        if (memcmp(info.data + i, g_poc.target_address, 6) == 0) {
                            pocLog("btdrv probe: target address at offset 0x%03X", i);
                            break;
                        }
                    }
                }
            }

            memcpy(previous_event, info.data, sizeof(previous_event));
            have_previous = true;

            if (type != BtdrvBleEventType_ScanResult)
                continue;

            scan_results++;
            total_scan_results++;

            if (!have_address) {
                scanned_address = info.scan_result.address;
                have_address = true;
            }

            if (scan_results <= 5) {
                pocLog("btdrv probe: scan result status=%u addr=%02X:%02X:%02X:%02X:%02X:%02X entries=%u rssi=%d",
                    info.scan_result.status, info.scan_result.address.address[0],
                    info.scan_result.address.address[1], info.scan_result.address.address[2],
                    info.scan_result.address.address[3], info.scan_result.address.address[4],
                    info.scan_result.address.address[5], info.scan_result.count,
                    info.scan_result.rssi);
            }
        }

        pocLog("btdrv probe: phase %u done fetches=%u empty=%u events=%u scan_results=%u", phase,
            fetches, empties, events, scan_results);

        btdrvStopBleScan();
    }

    btdrvClearBleScanFilters();
    pocLog("btdrv probe: done, %u scan result(s) in total", total_scan_results);

    // If the scan did produce a device, this is the one thing the earlier
    // rounds could never test: a connect in the same session, to an address the
    // stack has just seen. The configured address was never the problem, so a
    // failure here is about the stack's state, not about a stale address.
    if (have_address && client_if != 0xFF) {
        pocLog("btdrv probe: connect attempt to %02X:%02X:%02X:%02X:%02X:%02X (client_if=0x%02X)",
            scanned_address.address[0], scanned_address.address[1], scanned_address.address[2],
            scanned_address.address[3], scanned_address.address[4], scanned_address.address[5],
            client_if);
        rc = btdrvConnectGattServer(client_if, scanned_address, true, g_poc.aruid);
        pocLog("btdrv probe: ConnectGattServer rc=0x%08X", (u32)rc);
        pocDrainBleEvents("btdrv probe after ConnectGattServer", 3000u, NULL);
    } else {
        pocLog("btdrv probe: no scanned address to connect to (have_address=%u client_if=0x%02X)",
            have_address ? 1u : 0u, client_if);
    }

    eventClose(&event);
    btdrvExit();
}

// Drains btdrv's managed BLE event queue for a while and logs every non-empty
// event with its raw first 16 bytes.
//
// An empty queue is reported as "rc=0, type=0, payload all zero" (see the third
// hardware round in docs/ble-poc.md), so those reads are counted separately
// instead of being logged as events.
//
// When out_client_if is not NULL it collects the client interface of the first
// successful ClientRegistration: that is the interface to connect with, and it
// arrives as an event rather than in the dispatch result.
static u32 pocDrainBleEvents(const char* label, u32 duration_ms, u8* out_client_if)
{
    u32 deadline = pocNowMs() + duration_ms;
    u32 events = 0;
    u32 empties = 0;

    if (out_client_if != NULL)
        *out_client_if = 0xFFu;

    // Log only the first few events: the queue hands the same ClientRegistration
    // payload back over and over, and a flood of lines is what pushed the probe's
    // earlier output out of the log ring.
    while ((s32)(deadline - pocNowMs()) > 0 && !pocStopRequested() && events < 4) {
        BtdrvBleEventType type = (BtdrvBleEventType)0;
        Result rc;
        bool zeroed = true;

        memset(&g_ble_event, 0, sizeof(g_ble_event));
        rc = btdrvGetBleManagedEventInfo(&g_ble_event, sizeof(g_ble_event), &type);
        if (R_FAILED(rc)) {
            pocLog("%s: GetBleManagedEventInfo rc=0x%08X", label, (u32)rc);
            break;
        }

        for (u32 i = 0; i < 16; i++) {
            if (g_ble_event.data[i] != 0) {
                zeroed = false;
                break;
            }
        }

        if (type == (BtdrvBleEventType)0 && zeroed) {
            empties++;
            svcSleepThread(10000000ull); // 10ms, do not spin on an empty queue
            continue;
        }

        events++;
        pocLog("%s: event type=%u raw=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
            label, (u32)type,
            g_ble_event.data[0], g_ble_event.data[1], g_ble_event.data[2], g_ble_event.data[3],
            g_ble_event.data[4], g_ble_event.data[5], g_ble_event.data[6], g_ble_event.data[7],
            g_ble_event.data[8], g_ble_event.data[9], g_ble_event.data[10], g_ble_event.data[11],
            g_ble_event.data[12], g_ble_event.data[13], g_ble_event.data[14], g_ble_event.data[15]);

        if (type == BtdrvBleEventType_ClientRegistration) {
            pocLog("%s: ClientRegistration result=0x%08X client_if=0x%02X status=%u",
                label, g_ble_event.client_registration.result,
                g_ble_event.client_registration.client_if,
                g_ble_event.client_registration.status);
            if (out_client_if != NULL && g_ble_event.client_registration.result == 0)
                *out_client_if = g_ble_event.client_registration.client_if;
        } else if (type == BtdrvBleEventType_ClientConnection) {
            pocLog("%s: ClientConnection result=0x%08X status=%u client_if=0x%02X conn_id=%u "
                   "addr=%02X:%02X:%02X:%02X:%02X:%02X reason=0x%04X",
                label, g_ble_event.client_connection.result,
                g_ble_event.client_connection.status,
                g_ble_event.client_connection.client_if,
                g_ble_event.client_connection.conn_id,
                g_ble_event.client_connection.address.address[0],
                g_ble_event.client_connection.address.address[1],
                g_ble_event.client_connection.address.address[2],
                g_ble_event.client_connection.address.address[3],
                g_ble_event.client_connection.address.address[4],
                g_ble_event.client_connection.address.address[5],
                g_ble_event.client_connection.reason);
        } else if (type == BtdrvBleEventType_ScanResult) {
            // Log a wider window than the decoded fields: the firmware's payload
            // layout is not guaranteed to match libnx's (the request shapes
            // already do not), and a device address sitting at another offset is
            // exactly what "decoded everything zero" looked like before.
            pocLog("%s: ScanResult status=%u addr=%02X:%02X:%02X:%02X:%02X:%02X entries=%u",
                label, g_ble_event.scan_result.status,
                g_ble_event.scan_result.address.address[0], g_ble_event.scan_result.address.address[1],
                g_ble_event.scan_result.address.address[2], g_ble_event.scan_result.address.address[3],
                g_ble_event.scan_result.address.address[4], g_ble_event.scan_result.address.address[5],
                g_ble_event.scan_result.count);
            pocLog("%s: ScanResult raw=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X "
                   "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                label,
                g_ble_event.data[0], g_ble_event.data[1], g_ble_event.data[2], g_ble_event.data[3],
                g_ble_event.data[4], g_ble_event.data[5], g_ble_event.data[6], g_ble_event.data[7],
                g_ble_event.data[8], g_ble_event.data[9], g_ble_event.data[10], g_ble_event.data[11],
                g_ble_event.data[12], g_ble_event.data[13], g_ble_event.data[14], g_ble_event.data[15],
                g_ble_event.data[16], g_ble_event.data[17], g_ble_event.data[18], g_ble_event.data[19],
                g_ble_event.data[20], g_ble_event.data[21], g_ble_event.data[22], g_ble_event.data[23],
                g_ble_event.data[24], g_ble_event.data[25], g_ble_event.data[26], g_ble_event.data[27],
                g_ble_event.data[28], g_ble_event.data[29], g_ble_event.data[30], g_ble_event.data[31]);

            if (g_poc.use_target_address) {
                for (u32 i = 0; i + 6u <= sizeof(g_ble_event.data); i++) {
                    if (memcmp(g_ble_event.data + i, g_poc.target_address, 6) == 0) {
                        pocLog("%s: ScanResult contains the target address at offset 0x%X", label, i);
                        break;
                    }
                }
            }
        }
    }

    pocLog("%s: drained %u event(s), %u empty read(s)", label, events, empties);
    return events;
}

// btm:u requests with the applet's ARUID.
//
// libnx's btmu wrappers build their requests with appletGetAppletResourceUserId(),
// which is meaningless inside a sysmodule. The 2026-09-21 hardware logs show the
// consequence: btm accepted every scan and never delivered a result, and it
// refused the connect with its own 0x0005568F. These send the same commands with
// the ARUID the NRO reported on START instead (the shapes are libnx's, read from
// nx/source/services/btmu.c).
static Result pocBtmuStartSmartScan(u64 aruid, const BtdrvGattAttributeUuid* uuid)
{
    const struct {
        BtdrvGattAttributeUuid uuid;
        u32 pad;
        u64 aruid;
    } in = { *uuid, 0, aruid };

    return serviceDispatchIn(btmuGetServiceSession_IBtmUserCore(), 8, in, .in_send_pid = true);
}

static Result pocBtmuGetSmartScanResults(u64 aruid, BtdrvBleScanResult* results, u8 count, u8* total_out)
{
    return serviceDispatchInOut(btmuGetServiceSession_IBtmUserCore(), 10, aruid, *total_out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { results, sizeof(BtdrvBleScanResult) * count } },
        .in_send_pid = true,
    );
}

static Result pocBtmuBleConnect(u64 aruid, const BtdrvAddress* addr)
{
    const struct {
        BtdrvAddress addr;
        u8 pad[2];
        u64 aruid;
    } in = { *addr, { 0 }, aruid };

    return serviceDispatchIn(btmuGetServiceSession_IBtmUserCore(), 18, in, .in_send_pid = true);
}

static Result pocBtmuGetConnectionState(u64 aruid, BtdrvBleConnectionInfo* info, u8 count, u8* total_out)
{
    return serviceDispatchInOut(btmuGetServiceSession_IBtmUserCore(), 20, aruid, *total_out,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
        .buffers = { { info, sizeof(BtdrvBleConnectionInfo) * count } },
        .in_send_pid = true,
    );
}

// Scan and connect through btm:u, this time with an ARUID btm can match.
static void pocRunBtmuAruidProbe(PocWorker* w)
{
    BtdrvGattAttributeUuid uuid = pocUuid16(POC_UUID16_ADVERTISED_SERVICE);
    Result rc;
    u8 total = 0;
    bool found = false;

    (void)w;

    rc = pocBtmuStartSmartScan(g_poc.aruid, &uuid);
    pocLog("btmu: StartBleScanForSmartDevice(0x%04X, aruid=0x%X) rc=0x%08X",
        POC_UUID16_ADVERTISED_SERVICE, (u32)g_poc.aruid, (u32)rc);
    if (R_FAILED(rc))
        return;

    for (u32 i = 0; i < 20 && !pocStopRequested(); i++) {
        total = 0;
        memset(g_btmu_scan_results, 0, sizeof(g_btmu_scan_results));
        rc = pocBtmuGetSmartScanResults(g_poc.aruid, g_btmu_scan_results, 2, &total);

        if (R_SUCCEEDED(rc) && total > 0) {
            pocLog("btmu: scan total=%u rc=0x%08X", total, (u32)rc);
            for (u32 k = 0; k < total && k < 2; k++) {
                pocLog("btmu: scan[%u] addr=%02X:%02X:%02X:%02X:%02X:%02X",
                    k, g_btmu_scan_results[k].addr.address[0],
                    g_btmu_scan_results[k].addr.address[1],
                    g_btmu_scan_results[k].addr.address[2],
                    g_btmu_scan_results[k].addr.address[3],
                    g_btmu_scan_results[k].addr.address[4],
                    g_btmu_scan_results[k].addr.address[5]);
            }
            found = true;
            break;
        }

        if (i % 5 == 0)
            pocLog("btmu: scan poll %u rc=0x%08X total=%u", i, (u32)rc, total);

        svcSleepThread(500000000ull); // 500ms
    }

    rc = btmuStopBleScanForSmartDevice();
    pocLog("btmu: StopBleScanForSmartDevice rc=0x%08X", (u32)rc);

    if (!found)
        pocLog("btmu: no scan results");

    if (!g_poc.use_target_address) {
        pocLog("btmu: no target address, connect skipped");
        return;
    }

    {
        BtdrvAddress target;

        memset(&target, 0, sizeof(target));
        memcpy(target.address, g_poc.target_address, sizeof(target.address));

        rc = pocBtmuBleConnect(g_poc.aruid, &target);
        pocLog("btmu: BleConnect(aruid=0x%X) rc=0x%08X", (u32)g_poc.aruid, (u32)rc);

        for (u32 i = 0; i < 10 && !pocStopRequested(); i++) {
            total = 0;
            memset(g_btmu_connections, 0, sizeof(g_btmu_connections));
            rc = pocBtmuGetConnectionState(g_poc.aruid, g_btmu_connections, 2, &total);
            if (R_SUCCEEDED(rc) && total > 0) {
                pocLog("btmu: connection state total=%u rc=0x%08X handle=%u addr=%02X:%02X:%02X:%02X:%02X:%02X",
                    total, (u32)rc, g_btmu_connections[0].connection_handle,
                    g_btmu_connections[0].addr.address[0], g_btmu_connections[0].addr.address[1],
                    g_btmu_connections[0].addr.address[2], g_btmu_connections[0].addr.address[3],
                    g_btmu_connections[0].addr.address[4], g_btmu_connections[0].addr.address[5]);
                break;
            }
            svcSleepThread(500000000ull);
        }
    }
}

// Identity probe: the console side of docs/ble-re.md.
//
// The request shapes are settled now (every command case in the firmware's
// dispatcher has been read; libnx's shapes match), so what this probe is for is
// semantics: it reads back things a wrong call cannot fake - the adapter's own
// name, BD_ADDR, class of device, AFH channel map and BLE channel map - brings
// the manager up with InitializeBle, connects with the client_if the manager
// hands out, and logs the raw managed events, which is where the
// ClientRegistration and ClientConnection payloads become readable instead of
// inferred.
//
// Everything here is a read or a purely local registration: no BF write, no
// visibility/advertise change, no radio state change, nothing that reaches a
// DG-LAB device.
static void pocRunBtdrvIdentityProbe(PocWorker* w)
{
    BtdrvAdapterProperty property;
    Event event;
    Result rc;
    bool enabled = false;
    u8 client_if = 0xFFu;
    char name[0x30];

    pocStopScan(w);

    rc = btdrvInitialize();
    pocLog("identity: btdrvInitialize rc=0x%08X", (u32)rc);
    if (R_FAILED(rc))
        return;

    rc = btdrvIsBluetoothEnabled(&enabled);
    pocLog("identity: IsBluetoothEnabled rc=0x%08X value=%u", (u32)rc, enabled ? 1u : 0u);

    rc = btmInitialize();
    if (R_SUCCEEDED(rc)) {
        BtmState state = BtmState_NotInitialized;
        Result state_rc = btmGetState(&state);
        pocLog("identity: btmGetState rc=0x%08X state=%u", (u32)state_rc, (u32)state);
        btmExit();
    } else {
        pocLog("identity: btmInitialize rc=0x%08X", (u32)rc);
    }

    // Self-validating reads: a wrong command number cannot produce the console's
    // own name, MAC or a plausible channel bitmap.
    memset(&property, 0, sizeof(property));
    rc = btdrvGetAdapterProperty(BtdrvAdapterPropertyType_Address, &property);
    pocLog("identity: address rc=0x%08X size=%u %02X:%02X:%02X:%02X:%02X:%02X",
        (u32)rc, property.size,
        property.data[0], property.data[1], property.data[2],
        property.data[3], property.data[4], property.data[5]);

    memset(&property, 0, sizeof(property));
    rc = btdrvGetAdapterProperty(BtdrvAdapterPropertyType_Name, &property);
    if (R_SUCCEEDED(rc) && property.size > 0 && property.size < 0xF8u) {
        size_t size = property.size < sizeof(name) - 1 ? property.size : sizeof(name) - 1;
        memcpy(name, property.data, size);
        name[size] = '\0';
        pocLog("identity: name rc=0x%08X size=%u '%s'", (u32)rc, property.size, name);
    } else {
        pocLog("identity: name rc=0x%08X size=%u", (u32)rc, property.size);
    }

    memset(&property, 0, sizeof(property));
    rc = btdrvGetAdapterProperty(BtdrvAdapterPropertyType_ClassOfDevice, &property);
    pocLog("identity: class rc=0x%08X size=%u %02X%02X%02X",
        (u32)rc, property.size, property.data[2], property.data[1], property.data[0]);

    // BLE side. InitializeBle is what brings the manager up, and it is the
    // manager's own registration that hands out the client interface (client_if
    // 0x02 on the 2026-09-21 run) - an explicit RegisterGattClient before this
    // point answered 0x00029E71 and produced nothing.
    memset(&event, 0, sizeof(event));
    rc = btdrvInitializeBle(&event);
    pocLog("identity: InitializeBle rc=0x%08X", (u32)rc);
    if (R_FAILED(rc)) {
        btdrvExit();
        return;
    }

    pocDrainBleEvents("identity after InitializeBle", 2000u, &client_if);

    rc = btdrvEnableBle();
    pocLog("identity: EnableBle rc=0x%08X", (u32)rc);

    // The interface the manager handed out in the drain above is the one to
    // connect with; registering explicitly is both unnecessary and harmful (the
    // 2026-09-21 log shows it answering 0x37 / client_if=0xFF right after the
    // manager's own registration had succeeded).
    {
        pocLog("identity: client_if=0x%02X", client_if);

        // The manager binds the interface to this session, so the connection has
        // to happen here rather than in a later one.
        if (client_if != 0xFFu && g_poc.use_target_address) {
            BtdrvAddress target;

            memset(&target, 0, sizeof(target));
            memcpy(target.address, g_poc.target_address, sizeof(target.address));

            pocLog("identity: connect attempt to %02X:%02X:%02X:%02X:%02X:%02X (client_if=0x%02X)",
                target.address[0], target.address[1], target.address[2],
                target.address[3], target.address[4], target.address[5], client_if);

            rc = btdrvConnectGattServer(client_if, target, true, g_poc.aruid);
            pocLog("identity: ConnectGattServer rc=0x%08X", (u32)rc);
            pocDrainBleEvents("identity after ConnectGattServer", 6000u, NULL);

            // Two cheap parameter variants. The direct/ARUID pair is the one
            // libnx documents, but this module's bindings have already turned out
            // to be stale in several places, and the firmware answers 0x00029E71
            // (its own module Result) for the documented form.
            rc = btdrvConnectGattServer(client_if, target, false, g_poc.aruid);
            pocLog("identity: ConnectGattServer(indirect, aruid=0x%08X) rc=0x%08X",
                (u32)g_poc.aruid, (u32)rc);
            pocDrainBleEvents("identity after ConnectGattServer (indirect)", 3000u, NULL);

            rc = btdrvConnectGattServer(client_if, target, true, 0);
            pocLog("identity: ConnectGattServer(direct, aruid=0) rc=0x%08X", (u32)rc);
            pocDrainBleEvents("identity after ConnectGattServer (aruid 0)", 3000u, NULL);

            // The other "please connect to this address" entry point (command 23,
            // used for reconnecting a device the stack already knows). Cheap to
            // try: if the Bluetooth module's 0x14F means "no record for that
            // address yet", the answer here will be just as informative.
            rc = btdrvTriggerConnection(target, 0);
            pocLog("identity: TriggerConnection rc=0x%08X", (u32)rc);
            pocDrainBleEvents("identity after TriggerConnection", 3000u, NULL);
        } else if (!g_poc.use_target_address) {
            pocLog("identity: no target address (see dglab-ble-address.txt), connect skipped");
        } else {
            pocLog("identity: no client_if yet, connect skipped");
        }
    }

    // btm:u with an ARUID btm can match (see the note above pocBtmuStartSmartScan).
    pocRunBtmuAruidProbe(w);

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

        case DglabPocAction_ProbeBtdrvIdentity:
            // Same deferral as the scan probe.
            pocLog("action: btdrv identity probe");
            w->probe_identity = true;
            w->restart_scan = true;
            return true;

        case DglabPocAction_ScanWithCommonCompany:
            pocLog("action: control scan with a common company ID");
            w->forced_filter = POC_FILTER_CONTROL_COMPANY;
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
    u32 action;

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

    // The NRO sends START and the first ACTION back to back, so give that action
    // a moment to arrive before the first scan starts.
    //
    // The driver-level scan probe is no longer automatic: it calls
    // btdrvInitializeBle, and the BLE manager binds its internal connection to
    // the session that did that - once that session ends, every later BLE-side
    // command answers 0xF601 (KernelError_ConnectionClosed), which is what the
    // 2026-09-21 hardware logs in docs/ble-poc.md show. The Left key still runs
    // it on demand.
    svcSleepThread(300000000ull); // 300ms

    bool any_action = false;
    while (pocTakeAction(w, &action)) {
        any_action = true;
        pocHandleAction(w, action);
    }

    if (w->probe_identity) {
        w->probe_identity = false;
        g_identity_probe_pending = false;
        pocRunBtdrvIdentityProbe(w);
    } else if (g_identity_probe_pending && !any_action && !g_poc.skip_probes) {
        // First session after a boot: run the identity probe before anything
        // else touches BLE, so it answers on a clean state. Asking for another
        // action in that first session (scan variants, the Left probe) skips it
        // for now - the flag stays set, so the next quiet session still gets it.
        // A session started with DGLAB_POC_START_FLAG_SKIP_PROBES never gets it,
        // which is how a scan-only session can be observed on a clean console.
        g_identity_probe_pending = false;
        pocLog("identity: first session after boot, running the probe before any scan");
        pocRunBtdrvIdentityProbe(w);
    } else if (g_poc.skip_probes) {
        pocLog("probes: skipped for this session (START flag)");
    }

    if (w->probe_btdrv) {
        w->probe_btdrv = false;
        pocRunBtdrvScanProbe(w);
    }

    while (!pocStopRequested()) {
        BtdrvAddress address;

        while (pocTakeAction(w, &action)) {
            if (pocHandleAction(w, action))
                break;
        }

        if (w->probe_btdrv) {
            w->probe_btdrv = false;
            pocRunBtdrvScanProbe(w);
            continue;
        }

        if (w->probe_identity) {
            w->probe_identity = false;
            pocRunBtdrvIdentityProbe(w);
            continue;
        }

        w->restart_scan = false;

        if (g_poc.use_target_address) {
            memcpy(address.address, g_poc.target_address, sizeof(address.address));
            w->filter_used = 0;
            // The `found` milestone is deliberately NOT set here: a configured
            // address means "skip the scan", not "a scan returned the device".
            // Setting it made the status line claim a discovery that never
            // happened (2026-09-21 hardware round).
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

    mutexUnlock(&g_poc.mutex);

    // Wait for and release the live Thread, never a copy: libnx's threadClose()
    // refuses a thread that is still unregistering itself (LibnxError_BadInput,
    // 0x1759) and releases nothing, and a copy taken before the wait still has
    // the running thread's state in it - the worker's stack and its mirror
    // mapping would then stay behind for good (docs/dglab-socket.md, "反复启停后
    // 服务端起不来").
    if (g_poc.worker_thread.handle != INVALID_HANDLE) {
        Result wait_rc = threadWaitForExit(&g_poc.worker_thread);
        Result close_rc;

        if (R_FAILED(wait_rc))
            pocLog("worker did not exit, rc=0x%08X", (u32)wait_rc);

        close_rc = threadClose(&g_poc.worker_thread);

        if (R_FAILED(close_rc))
            pocLog("worker close rc=0x%08X (its stack was not released)", (u32)close_rc);
    }

    mutexLock(&g_poc.mutex);
    memset(&g_poc.worker_thread, 0, sizeof(g_poc.worker_thread));
    memset(&g_poc.status, 0, sizeof(g_poc.status));

    // The ring is NOT cleared here: the NRO's reader polls every frame, and
    // wiping the ring at session start threw away whatever the previous session
    // (or the probe that runs in this one) had written but not been read yet -
    // the 2026-09-21 hardware round lost the whole identity probe that way.
    // log_write_offset stays monotonic and readers only take what is newer than
    // log_valid_from, so old bytes are simply never handed out again.
    g_poc.log_valid_from = g_poc.log_write_offset;

    g_poc.status.state = DglabPocState_Initializing;
    g_poc.status.auto_write = g_poc.auto_write ? 1u : 0u;
    g_poc.pending_action = 0;
    g_poc.stop_requested = false;
    g_poc.aruid = request->applet_resource_user_id;
    g_poc.status.aruid_low = (u32)g_poc.aruid;
    g_poc.use_target_address = (request->flags & DGLAB_POC_START_FLAG_TARGET_ADDRESS) != 0;
    g_poc.skip_probes = (request->flags & DGLAB_POC_START_FLAG_SKIP_PROBES) != 0;
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
