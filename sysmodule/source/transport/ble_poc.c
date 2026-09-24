// BLE transport proof of concept.
//
// This is the only place in the project that talks to the Switch Bluetooth
// stack, and it runs in the sysmodule: the NRO never touches BLE itself.
//
// Transport choice:
//   Three routes have been tried, all documented in docs/ble-poc.md:
//   btdrv directly (scans fine, the connect comes back Bluetooth/0x1806), the
//   btdev wrapper (bt + btm:u, applet-only: a foreign ARUID is answered with
//   Sf/0x60A), and - v18 - the base `btm` service, which this process may open
//   and which carries the same BLE surface without the applet restriction
//   (pocRunBtmBleProbe). btdev still drives the normal scan/connect session.
//
// Scan filter:
//   Which service UUID the Coyote 3.0 advertises is still not settled (0x1812
//   in early firmware, 0x180C per the phone after a device firmware update, no
//   service UUID at all in the 2026-09-22 dump), so the default scan tries the
//   advertised UUID first and falls back through the protocol UUID to btm's
//   manufacturer-data filter. The probe dumps the AD structures instead of
//   trusting either reading.
//
// Packet construction uses dglab/protocol/coyote_v3.h; this file only moves
// bytes and reports what happened through the log ring the NRO reads.

#include <dglab/transport/ble_poc.h>

#include <dglab/protocol/coyote_v3.h>
#include <dglab/protocol/coyote_v3_session.h>

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

// The driver-level probe's phone window: how long it keeps scanning to find the
// target before giving up, and how long it keeps watching after the target
// shows up (the user reads "TAP CONNECT ON THE PHONE NOW" on the console screen).
#define POC_PROBE_FIND_MS 15000u
#define POC_PROBE_PHONE_WINDOW_MS 20000u

// Service UUID the Coyote puts into its advertisement. It used to be 0x1812
// (the HID service); after a DG-LAB device firmware update the phone scanner
// shows 0x180C in the advertisement instead (2026-09-21, confirmed by the user
// against the device's own address). See docs/ble-poc.md.
#define POC_UUID16_ADVERTISED_SERVICE 0x180Cu

// The same UUID as it was before that firmware update: kept so the driver-level
// probe can show that a filter on the old value finds nothing.
#define POC_UUID16_LEGACY_ADVERTISED_SERVICE 0x1812u

// Company id in the advertisement's manufacturer specific data (AD type 0xFF)
// as the phone scanner reports it after the device's firmware update. It is the
// BLE module vendor's id - SIG company identifier 0x000A is "Qualcomm
// Technologies International (QTIL)", the former CSR - not DG-LAB's.
#define POC_ADVERTISED_COMPANY_ID 0x000Au

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
    bool probe_btm;
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

// Which probes may run on their own has changed twice on 2026-09-22:
//
//  - the identity probe used to run in the first session after a boot. That
//    question ("do libnx's btdrv command numbers match this firmware") is
//    settled, so it is manual-only now: press Right.
//  - the base-btm probe then took over the first session, so nobody had to find
//    the right key. It is manual-only again: the 2026-09-22 btm round ended in a
//    btm abort (the crash report is in the user's downloads), and a probe that
//    can leave btm holding unfinished work must not run unattended at boot.
//
// The flag below remembers that the identity probe still has not run since the
// boot, so the first quiet session still gets it - that one is read-only.
static bool g_identity_probe_pending = true;

// The managed BLE event payload is 0x400 bytes. It lives in .bss instead of on
// the worker's stack because tests/stack exists to fail new KB-scale frames
// (sysmodule/AGENTS.md, "线程与栈"); only the PoC worker thread touches it.
static BtdrvBleEventInfo g_ble_event;

// btm:u scan/connection results for the ARUID probe below. BtdrvBleScanResult is
// 0x148 bytes each, so two of them stay out of the worker's frame on purpose.
static BtdrvBleScanResult g_btmu_scan_results[2];
static BtdrvBleConnectionInfo g_btmu_connections[2];

// The base `btm` probe scans its own way (see pocRunBtmBleProbe): ten scan
// records, four connection entries and the GATT services of one connection.
// BtdrvBleScanResult is 0x148 bytes and BtmGattService 0x24, so all of it lives
// in .bss and never in a worker frame (sysmodule/AGENTS.md, "线程与栈").
#define POC_BTM_SCAN_MAX 10u
#define POC_BTM_SERVICE_MAX 12u
#define POC_BTM_CHARACTERISTIC_MAX 8u
static BtdrvBleScanResult g_btm_scan_results[POC_BTM_SCAN_MAX];
static BtdrvBleConnectionInfo g_btm_connections[4];
static BtmGattService g_btm_services[POC_BTM_SERVICE_MAX];
static BtmGattCharacteristic g_btm_characteristics[POC_BTM_CHARACTERISTIC_MAX];

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

// 16-bit UUIDs print as their value, everything else as bytes. 0x180C / 0x150A /
// 0x150B are 16-bit values, and those are what the BLE work is looking for.
static void pocUuidText(char* out, size_t out_size, const BtdrvGattAttributeUuid* uuid)
{
    if (out_size == 0)
        return;

    if (uuid->size == 2) {
        snprintf(out, out_size, "0x%04X",
            (unsigned)(uuid->uuid[0] | (uuid->uuid[1] << 8)));
    } else if (uuid->size == 4) {
        snprintf(out, out_size, "0x%08X",
            (unsigned)(uuid->uuid[0] | (uuid->uuid[1] << 8) | (uuid->uuid[2] << 16) |
                (uuid->uuid[3] << 24)));
    } else {
        pocHex(out, out_size, uuid->uuid, sizeof(uuid->uuid));
    }
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

// One line per 16 bytes, as four little-endian words. The firmware's GATT
// structures carry fields libnx does not decode (every characteristic's
// property byte reads back 0x00), so the raw bytes are what has to be looked at
// - but a 0x24-byte hex soup per entry would eat the 16 KB log ring that the
// transport lines need.
static void pocLogWords(const char* label, const u8* data, u32 size, u32 from)
{
    for (u32 offset = from; offset < size; offset += 16u) {
        u8 chunk[16] = { 0 };
        u32 word[4] = { 0, 0, 0, 0 };
        u32 avail = (size - offset) < 16u ? (size - offset) : 16u;

        memcpy(chunk, data + offset, avail);
        for (u32 w = 0; w < 4u; w++)
            memcpy(&word[w], chunk + w * 4u, 4u);

        pocLog("%s +%03X %08X %08X %08X %08X", label, (unsigned)offset,
            (unsigned)word[0], (unsigned)word[1], (unsigned)word[2], (unsigned)word[3]);
    }
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
// Defined next to the btm probe, used by the driver-level probe's device dump.
static void pocLogAdStructures(const char* label, const u8* data, size_t size);
static void pocLogAdArray(const char* label, const BtdrvBleAdvertisement* list, u32 count);

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
// The connect call answered Bluetooth/0x1806 in every state tried so far (clean
// client_if, device just scanned, scan stopped). That Result is the mapping of
// the message layer's status 0xC8 = "this client_if has no connection context"
// (docs/ble-re.md), which is what pocRegisterGattClientAndConnect above now
// addresses. The matrix is kept as the control: the same variants, with the
// interface the manager handed out, so the two can be compared in one log.
// Control connect: 0xFF is never a valid interface, so this can only be refused.
// The refusal code tells the two states apart (docs/ble-re.md):
//   Bluetooth/0x1806 - the request reached the BLE thread and came back
//                      ("this interface has no connection context");
//   Bluetooth/0x14F  - the message layer itself answered, which is what its
//                      "no free task slot" status (0x72) maps to.
// Called between the steps that touch BLE, so the step that exhausts the
// message layer shows up as the 0x1806 -> 0x14F transition.
static void pocControlConnect(const char* label)
{
    BtdrvAddress addr;
    Result rc;

    if (!g_poc.use_target_address) {
        pocLog("%s: control connect skipped, no configured address", label);
        return;
    }

    memset(&addr, 0, sizeof(addr));
    memcpy(addr.address, g_poc.target_address, sizeof(addr.address));

    rc = btdrvConnectGattServer(0xFFu, addr, true, 0);
    pocLog("%s: control connect client_if=0xFF rc=0x%08X", label, (u32)rc);
}

// The user-side BLE event channel (`bt` service).
//
// libnx's btGetLeEventInfo is "identical to btdrvGetLeHidEventInfo except
// different state is used" - there are two event channels, and the btdrv one we
// poll is not the one the applet side (btdev) reads. The registration event
// never showed up on the btdrv side (docs/ble-re.md), so the probe also opens
// `bt` and drains that channel around the registration and the connect.
static bool g_bt_open;
static Event g_bt_event;
static bool g_bt_event_open;

static void pocBtEventsOpen(void)
{
    Result rc;

    if (g_bt_open)
        return;

    rc = btInitialize();
    g_bt_open = R_SUCCEEDED(rc);
    pocLog("btdrv probe: btInitialize rc=0x%08X", (u32)rc);

    if (!g_bt_open)
        return;

    memset(&g_bt_event, 0, sizeof(g_bt_event));
    rc = btRegisterBleEvent(&g_bt_event);
    g_bt_event_open = R_SUCCEEDED(rc);
    pocLog("btdrv probe: btRegisterBleEvent rc=0x%08X", (u32)rc);
}

static void pocBtEventsClose(void)
{
    if (g_bt_event_open) {
        eventClose(&g_bt_event);
        g_bt_event_open = false;
    }

    if (g_bt_open) {
        btExit();
        g_bt_open = false;
    }
}

// ---------------------------------------------------------------------------
// btm transport: the connection btm hands out, driven through the bt service
// ---------------------------------------------------------------------------
//
// btm owns the connection and the GATT table (btmGetGattServices); the actual
// GATT client traffic goes through the `bt` service, which is the pairing
// Nintendo designed for it: btLeClientWriteCharacteristic for B0/BF and
// btLeClientRegisterNotification + btGetLeEventInfo for the B1 answers.
//
// The protocol layer (dglab/protocol/coyote_v3_session.h) does the packet
// construction; this file only moves bytes, as sysmodule/AGENTS.md requires.

typedef struct {
    bool connected;
    u32 handle;
    BtdrvGattId service;      // 0x180C
    BtdrvGattId write_char;   // 0x150A
    BtdrvGattId notify_char;  // 0x150B
    bool notify_registered;
    DglabCoyoteV3Session session;
    u32 last_tick_ms;
    u32 last_rearm_ms;
    u32 start_ms;
    u32 writes;
    u32 notifications;
    u32 b1_count;
    // The queue replays "the current record" until something else arrives, so
    // the pump keeps the last one it saw and dumps only changes. The comparison
    // covers the whole 0x50-byte prefix: a client_notify record begins with the
    // same {result, conn_id} as the connection_update record that sits in this
    // state, so the first eight bytes cannot tell them apart.
    u32 events_seen;
    u8 last_event[0x50];
} PocBtmTransport;

// The session holds two 128-entry waveform channels, so it lives in .bss.
static PocBtmTransport g_btm_transport;

// Discovered during the GATT table walk so the transport can use it.
static BtmGattService g_btm_proto_service;
static BtmGattCharacteristic g_btm_proto_write;
static BtmGattCharacteristic g_btm_proto_notify;
static bool g_btm_proto_ready;
static BtmGattService g_btm_battery_service;
static BtmGattCharacteristic g_btm_battery_char;
static bool g_btm_battery_ready;
static bool g_btm_battery_read_sent;

// The client characteristic configuration descriptor (0x2902) under 0x150B:
// btm's descriptor struct does not decode the instance id libnx's GATT calls
// want, so it is taken from +0x1C (where it sits in the characteristic struct)
// and the guess is backed by the raw dump. Reading the CCCD back after
// RegisterNotification is what says whether the subscription reached the device.
static BtdrvGattId g_btm_cccd;
static bool g_btm_cccd_ready;

// Managed-queue peek (see pocBtmManagedPeek): another 0x400-byte payload, so it
// lives in .bss like g_ble_event.
#define POC_BTM_EVENT_DUMP_MAX 6u
static BtdrvBleEventInfo g_managed_event;
static u8 g_managed_last[0x50];
static u32 g_managed_seen;

static void pocBtmLinkWrite(void* context, const u8* data, size_t size)
{
    PocBtmTransport* transport = context;
    Result rc;
    char hex[3u * 24u + 1u];

    if (!transport->connected)
        return;

    rc = btLeClientWriteCharacteristic(transport->handle, true, &transport->service,
        &transport->write_char, data, size, BtdrvGattAuthReqType_None, false);

    if (R_FAILED(rc)) {
        // The characteristic's property byte came back as 0x00, so the write
        // type is a guess: fall back to write-with-response once and report it.
        Result retry = btLeClientWriteCharacteristic(transport->handle, true,
            &transport->service, &transport->write_char, data, size,
            BtdrvGattAuthReqType_None, true);

        pocLog("btm transport: write without response failed (0x%08X), with response rc=0x%08X",
            (u32)rc, (u32)retry);
        rc = retry;
    }

    pocHex(hex, sizeof(hex), data, size < 24u ? size : 24u);

    // Every write would flood the ring; the first few and every tenth after that
    // are enough to read the cadence and the packet contents.
    if (transport->writes < 3u || (transport->writes % 10u) == 0u)
        pocLog("btm transport: write %u byte(s) %s rc=0x%08X", (unsigned)size, hex, (u32)rc);

    if (R_SUCCEEDED(rc))
        transport->writes++;
}

static bool pocBtmTransportStart(u32 handle)
{
    DglabCoyoteV3Link link = { pocBtmLinkWrite, &g_btm_transport };
    DglabCoyoteV3SessionConfig config;
    Result rc;

    memset(&g_btm_transport, 0, sizeof(g_btm_transport));

    g_btm_transport.handle = handle;
    g_btm_transport.service.instance_id = (u8)g_btm_proto_service.instance_id;
    g_btm_transport.service.uuid = g_btm_proto_service.uuid;
    g_btm_transport.write_char.instance_id = (u8)g_btm_proto_write.instance_id;
    g_btm_transport.write_char.uuid = g_btm_proto_write.uuid;
    g_btm_transport.notify_char.instance_id = (u8)g_btm_proto_notify.instance_id;
    g_btm_transport.notify_char.uuid = g_btm_proto_notify.uuid;

    // BF test values: both soft limits are written as 0, which caps every
    // channel at strength 0 - the device cannot output anything while this
    // transport is being verified, whatever a B0 packet asks for. The official
    // app rewrites BF on every connect, so this does not leave the device in a
    // state the user cannot get out of. The real implementation takes these from
    // the app's own configuration.
    memset(&config, 0, sizeof(config));
    config.bf.soft_limit_a = 0;
    config.bf.soft_limit_b = 0;
    dglabCoyoteV3SessionInit(&g_btm_transport.session, &link, &config);

    rc = btLeClientRegisterNotification(handle, true, &g_btm_transport.service,
        &g_btm_transport.notify_char);
    pocLog("btm transport: RegisterNotification(0x150B) rc=0x%08X", (u32)rc);
    g_btm_transport.notify_registered = R_SUCCEEDED(rc);

    // RegisterNotification only says "accepted". If it really subscribed, the
    // CCCD under 0x150B reads back as 0x0001; if it did not, the device has no
    // reason to ever notify and the whole B1 wait is pointless. The answer
    // arrives through the same event channel as everything else.
    if (g_btm_cccd_ready) {
        Result cccd_rc = btLeClientReadDescriptor(handle, true, &g_btm_transport.service,
            &g_btm_transport.notify_char, &g_btm_cccd, BtdrvGattAuthReqType_None);

        pocLog("btm transport: ReadDescriptor(CCCD 0x2902 id=%u) rc=0x%08X",
            (unsigned)g_btm_cccd.instance_id, (u32)cccd_rc);
    } else {
        pocLog("btm transport: no CCCD entry from btm, subscription cannot be read back");
    }

    g_btm_transport.connected = true;
    g_btm_transport.last_tick_ms = pocNowMs();
    g_btm_transport.start_ms = g_btm_transport.last_tick_ms;
    g_btm_battery_read_sent = false;

    // OnConnected writes the BF packet (soft limits) before any B0.
    dglabCoyoteV3SessionOnConnected(&g_btm_transport.session);

    // Drive both channels to an absolute zero before anything else: it makes the
    // device's output state explicit (nothing can pulse after this) and the
    // resulting B0 carries a non-zero sequence number, so the device answers
    // with a B1 - which is what proves the notification path works.
    dglabCoyoteV3SessionSetStrengthZero(&g_btm_transport.session, DglabCoyoteV3ChannelA);
    dglabCoyoteV3SessionSetStrengthZero(&g_btm_transport.session, DglabCoyoteV3ChannelB);

    return true;
}

// Drains the user-side channel and ticks the session for duration_ms.
static void pocBtmTransportPump(u32 duration_ms)
{
    u32 deadline = pocNowMs() + duration_ms;

    while ((s32)(deadline - pocNowMs()) > 0 && !pocStopRequested()) {
        for (u32 i = 0; i < 8u; i++) {
            BtdrvBleEventType type = (BtdrvBleEventType)0;
            const u8* notify;
            u16 size;
            Result rc;
            bool zero = true;

            memset(&g_ble_event, 0, sizeof(g_ble_event));
            rc = btGetLeEventInfo(&g_ble_event, sizeof(g_ble_event), &type);

            if (R_FAILED(rc))
                break;

            // The whole prefix decides whether this is an event at all: a
            // record whose first eight bytes are zero can still carry something
            // further in (the notify payload sits at +0x4A).
            for (u32 b = 0; b < sizeof(g_btm_transport.last_event); b++) {
                if (g_ble_event.data[b] != 0)
                    zero = false;
            }

            if (zero)
                break;

            // Only what changed is worth a line (the queue hands the same record
            // back until something else arrives), but what changed is worth all
            // of it: the 2026-09-25 01:38 run deduplicated on eight bytes and
            // dumped the first three records only, which is exactly the part a
            // client_notify shares with the connection_update record sitting in
            // this state - a notification arriving later could not have shown up.
            if (memcmp(g_btm_transport.last_event, g_ble_event.data,
                    sizeof(g_btm_transport.last_event)) != 0) {
                memcpy(g_btm_transport.last_event, g_ble_event.data,
                    sizeof(g_btm_transport.last_event));
                g_btm_transport.events_seen++;

                if (g_btm_transport.events_seen <= POC_BTM_EVENT_DUMP_MAX) {
                    char raw_label[32];
                    u32 size_field = (u32)g_ble_event.data[0x48] |
                        ((u32)g_ble_event.data[0x49] << 8);
                    u32 conn_field = (u32)g_ble_event.data[4] |
                        ((u32)g_ble_event.data[5] << 8) | ((u32)g_ble_event.data[6] << 16) |
                        ((u32)g_ble_event.data[7] << 24);

                    snprintf(raw_label, sizeof(raw_label), "btm transport: ev#%u",
                        (unsigned)g_btm_transport.events_seen);
                    pocLogWords(raw_label, g_ble_event.data, 0x50u, 0u);
                    pocLog("%s meta size@0x48=%u conn@0x04=%u byte@0x08=%u word@0x0C=%u",
                        raw_label, (unsigned)size_field, (unsigned)conn_field,
                        (unsigned)g_ble_event.data[8],
                        (unsigned)((u32)g_ble_event.data[0x0C] |
                            ((u32)g_ble_event.data[0x0D] << 8) |
                            ((u32)g_ble_event.data[0x0E] << 16) |
                            ((u32)g_ble_event.data[0x0F] << 24)));
                }
            }

            // The type the firmware reports is not usable, so the notification
            // is recognised by its shape: the payload sits at +0x4A and its
            // length at +0x48 (libnx's client_notify layout).
            size = (u16)(g_ble_event.data[0x48] | (g_ble_event.data[0x49] << 8));
            notify = g_ble_event.data + 0x4A;

            if (size == 0u || size > 0x20u) {
                // Not a notify by libnx's layout: the dump above already shows
                // what it is, so this path stays quiet.
                continue;
            }

            g_btm_transport.notifications++;

            {
                char hex[3u * 0x20u + 1u];

                pocHex(hex, sizeof(hex), notify, size);
                pocLog("btm transport: notify #%u size=%u %s",
                    g_btm_transport.notifications, (unsigned)size, hex);
            }

            if (notify[0] == DGLAB_COYOTE_V3_HEADER_B1) {
                DglabCoyoteV3B1 b1;

                g_btm_transport.b1_count++;

                if (dglabCoyoteV3DecodeB1(notify, size, &b1)) {
                    pocLog("btm transport: B1 sequence=%u strength A=%u B=%u", b1.sequence,
                        b1.strength_a, b1.strength_b);
                }
            }

            dglabCoyoteV3SessionOnNotification(&g_btm_transport.session, notify, size);
        }

        {
            u32 now = pocNowMs();
            u32 elapsed = now - g_btm_transport.last_tick_ms;

            if (elapsed > 0u) {
                g_btm_transport.last_tick_ms = now;
                dglabCoyoteV3SessionTick(&g_btm_transport.session, elapsed);
            }

            // The strength zero is the one command this test wants to see
            // answered. The reference algorithm waits for the B1 forever; while
            // nothing has come back, release the gate every 1.5s and let the
            // next packet carry the request again, so a lost packet (or a
            // subscription that had not settled yet) cannot stall the run.
            if (g_btm_transport.b1_count == 0u &&
                dglabCoyoteV3StrengthStateIsWaiting(&g_btm_transport.session.strength) &&
                (s32)(now - g_btm_transport.last_rearm_ms) >= 1500) {
                DglabCoyoteV3B1 synthetic;

                g_btm_transport.last_rearm_ms = now;
                synthetic.sequence = g_btm_transport.session.strength.inflight_sequence;
                synthetic.strength_a = 0;
                synthetic.strength_b = 0;
                dglabCoyoteV3StrengthStateOnB1(&g_btm_transport.session.strength, &synthetic);
                pocLog("btm transport: no B1 yet, sending the zero request again");
            }

            // One battery read per connection: the answer has to arrive through
            // the same event channel the B1 would use, so it says whether the
            // notification/event path works at all (and it cannot drive output).
            if (!g_btm_battery_read_sent && g_btm_battery_ready &&
                (s32)(now - g_btm_transport.start_ms) >= 1000) {
                BtdrvGattId service_id;
                BtdrvGattId char_id;
                Result rc;

                g_btm_battery_read_sent = true;
                memset(&service_id, 0, sizeof(service_id));
                service_id.instance_id = (u8)g_btm_battery_service.instance_id;
                service_id.uuid = g_btm_battery_service.uuid;
                memset(&char_id, 0, sizeof(char_id));
                char_id.instance_id = (u8)g_btm_battery_char.instance_id;
                char_id.uuid = g_btm_battery_char.uuid;

                rc = btLeClientReadCharacteristic(g_btm_transport.handle, true, &service_id,
                    &char_id, BtdrvGattAuthReqType_None);
                pocLog("btm transport: ReadCharacteristic(battery 0x1500) rc=0x%08X", (u32)rc);
            }
        }

        svcSleepThread(10000000ull); // 10ms
    }
}

// Last thing before the disconnect: does btdrv's *managed* queue carry the same
// records the `bt` channel shows? This probe has stayed away from btdrv on
// purpose (docs/history.md §28) - what broke the connect was a second
// InitializeBle/EnableBle and a RegisterGattClient from this process, not
// opening the service - so the peek runs after the transport window, when the
// measurement is already in the log, and it only reads.
static void pocBtmManagedPeek(void)
{
    Result rc = btdrvInitialize();

    pocLog("btm transport: managed peek, btdrvInitialize rc=0x%08X", (u32)rc);
    if (R_FAILED(rc))
        return;

    for (u32 i = 0; i < 8u; i++) {
        BtdrvBleEventType type = (BtdrvBleEventType)0;
        bool nonzero = false;

        memset(&g_managed_event, 0, sizeof(g_managed_event));
        rc = btdrvGetBleManagedEventInfo(&g_managed_event, sizeof(g_managed_event), &type);
        if (R_FAILED(rc)) {
            pocLog("btm transport: managed peek read rc=0x%08X", (u32)rc);
            break;
        }

        for (u32 b = 0; b < sizeof(g_managed_last); b++) {
            if (g_managed_event.data[b] != 0)
                nonzero = true;
        }

        if (!nonzero)
            break;

        if (memcmp(g_managed_last, g_managed_event.data, sizeof(g_managed_last)) == 0)
            continue;

        memcpy(g_managed_last, g_managed_event.data, sizeof(g_managed_last));
        g_managed_seen++;

        if (g_managed_seen <= 4u) {
            char raw_label[48];

            snprintf(raw_label, sizeof(raw_label), "btm transport: managed#%u type=%u",
                (unsigned)g_managed_seen, (unsigned)type);
            pocLogWords(raw_label, g_managed_event.data, 0x50u, 0u);
        }
    }

    pocLog("btm transport: managed peek done, %u distinct record(s)",
        (unsigned)g_managed_seen);
    btdrvExit();
}

static void pocBtmTransportStop(void)
{
    if (!g_btm_transport.connected)
        return;

    dglabCoyoteV3SessionOnDisconnected(&g_btm_transport.session);

    if (g_btm_transport.notify_registered) {
        Result rc = btLeClientDeregisterNotification(g_btm_transport.handle, true,
            &g_btm_transport.service, &g_btm_transport.notify_char);

        pocLog("btm transport: DeregisterNotification rc=0x%08X", (u32)rc);
        g_btm_transport.notify_registered = false;
    }

    pocBtmManagedPeek();

    pocLog("btm transport: done, writes=%u notify=%u b1=%u", g_btm_transport.writes,
        g_btm_transport.notifications, g_btm_transport.b1_count);
    g_btm_transport.connected = false;
}

// Drains the user-side channel and prints every payload it holds. A payload with
// byte 5 == 1 is the registration event, with the client_if in byte 4.
static u8 pocBtEventsDrain(const char* label, u32 rounds, u8* out_client_if)
{
    u8 found = 0xFFu;
    // The queue hands the same record back on every read; only a payload that
    // differs from the last one that was logged is worth a line (the identical
    // repeats filled the log ring on 2026-09-25).
    static u8 last_head[16];
    static bool have_last_head;

    if (!g_bt_open)
        return found;

    for (u32 i = 0; i < rounds && !pocStopRequested(); i++) {
        BtdrvBleEventType type = (BtdrvBleEventType)0;
        Result rc;
        bool zero = true;

        // The managed event payload is 0x400 bytes, so it lives in .bss (the
        // stack test fails any frame over 1 KB).
        memset(&g_ble_event, 0, sizeof(g_ble_event));
        rc = btGetLeEventInfo(&g_ble_event, sizeof(g_ble_event), &type);

        for (u32 b = 0; b < 8u; b++) {
            if (g_ble_event.data[b] != 0)
                zero = false;
        }

        if (R_FAILED(rc) || zero)
            continue;

        if (have_last_head && memcmp(last_head, g_ble_event.data, sizeof(last_head)) == 0)
            continue;

        memcpy(last_head, g_ble_event.data, sizeof(last_head));
        have_last_head = true;

        // Two rows, not one: the interesting events carry more than the first
        // eight bytes (the client connection style event puts the device address
        // at +0x0C), and the earlier one-line dump hid exactly that.
        pocLog("%s: bt event type=%u head %02X%02X%02X%02X %02X%02X%02X%02X "
               "%02X%02X%02X%02X %02X%02X%02X%02X", label, (u32)type,
            g_ble_event.data[0], g_ble_event.data[1], g_ble_event.data[2],
            g_ble_event.data[3], g_ble_event.data[4], g_ble_event.data[5],
            g_ble_event.data[6], g_ble_event.data[7], g_ble_event.data[8],
            g_ble_event.data[9], g_ble_event.data[10], g_ble_event.data[11],
            g_ble_event.data[12], g_ble_event.data[13], g_ble_event.data[14],
            g_ble_event.data[15]);

        if (g_poc.use_target_address) {
            for (u32 off = 0; off + 6u <= sizeof(g_ble_event.data); off++) {
                if (memcmp(g_ble_event.data + off, g_poc.target_address, 6) == 0) {
                    pocLog("%s: bt event carries the target address at +0x%X", label, off);
                    break;
                }
            }
        }

        // Same bytes read as libnx's client_connection event, so the log says
        // what it is instead of leaving it to be decoded by hand:
        //   {u32 result; u8 status; u8 client_if; u8 pad[2]; u32 conn_id;
        //    BtdrvAddress address; u16 reason}
        // status 0 = connected, 2 = disconnected (libnx's btdrv.h).
        pocLog("%s: bt event as connection: result=0x%08X status=%u client_if=%u "
               "conn_id=0x%08X addr=%02X:%02X:%02X:%02X:%02X:%02X reason=0x%04X",
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

        if (g_ble_event.data[5] == 1u && g_ble_event.data[4] != 0xFFu) {
            found = g_ble_event.data[4];
            pocLog("%s: bt registration event -> client_if=0x%02X", label, found);

            if (out_client_if != NULL)
                *out_client_if = found;
        }
    }

    return found;
}

// Polls btdrv's managed queue for a connection record that says the link to this
// address is up (status 0). This is the honest signal: with the marker edit
// removed a refused connect answers Bluetooth/0x1806 again, while a link the
// stack really made shows up here as
//   {u32 result; u8 status(0 = connected); u8 client_if; u16 pad; u32 conn_id;
//    BtdrvAddress address; u16 reason}
// (libnx's client_connection layout). Returns the connection id or 0xFFFFFFFF.
static u32 pocDrainBleConnectionEvent(const BtdrvAddress* addr, u32 timeout_ms)
{
    u32 deadline = pocNowMs() + timeout_ms;
    u32 conn_id = 0xFFFFFFFFu;

    while ((s32)(deadline - pocNowMs()) > 0 && !pocStopRequested()) {
        BtdrvBleEventType type = (BtdrvBleEventType)0;
        Result rc;
        bool zero = true;

        memset(&g_ble_event, 0, sizeof(g_ble_event));
        rc = btdrvGetBleManagedEventInfo(&g_ble_event, sizeof(g_ble_event), &type);

        if (R_FAILED(rc))
            break;

        for (u32 b = 0; b < 8u; b++) {
            if (g_ble_event.data[b] != 0)
                zero = false;
        }

        if (zero)
            continue;

        if (g_ble_event.client_connection.status == 0u &&
            g_ble_event.client_connection.conn_id != 0xFFFFFFFFu &&
            memcmp(g_ble_event.client_connection.address.address, addr->address, 6) == 0) {
            conn_id = g_ble_event.client_connection.conn_id;
            pocLog("btdrv probe: connected! conn_id=%u client_if=%u", conn_id,
                g_ble_event.client_connection.client_if);
            break;
        }
    }

    return conn_id;
}

// Finds the interface our own registration created by trying the candidates.
//
// The registration event is posted to a queue this process does not read (it
// never showed up in the managed-event queue, docs/ble-re.md), so the interface
// number has to be discovered. A connect on an interface that is not registered
// comes back as Bluetooth/0x14F, one without a connection context as
// Bluetooth/0x1806; an interface that returns 0 is one the stack accepted.
static u8 pocFindClientIfByConnect(const BtdrvAddress* addr)
{
    u8 found = 0xFFu;
    Result rc;

    // Control first: 0xFF is never a valid interface. If this also answers
    // Bluetooth/0x14F then 0x14F is not about the interface but about the
    // message layer (its sender answers 0x72 = "no free task slot" when the BLE
    // thread has none, and 0x72 maps to the same Result). If it answers
    // Bluetooth/0x1806 while 0..3 answer 0x14F, then 0x14F really is per
    // interface and the candidates are in the manager's table.
    rc = btdrvConnectGattServer(0xFFu, *addr, true, 0);
    pocLog("btdrv probe: control connect client_if=0xFF rc=0x%08X", (u32)rc);

    for (u8 candidate = 0; candidate < 4u && !pocStopRequested(); candidate++) {
        rc = btdrvConnectGattServer(candidate, *addr, true, 0);

        pocLog("btdrv probe: probe connect client_if=0x%02X rc=0x%08X", candidate, (u32)rc);

        // The result code alone is not enough - a client without a connection
        // context is answered with an error, but only a connection record for
        // this address proves that the link actually came up.
        if (pocDrainBleConnectionEvent(addr, 2000u) != 0xFFFFFFFFu) {
            found = candidate;
            break;
        }

        if (rc == 0)
            found = candidate;
    }

    if (found != 0xFFu)
        pocLog("btdrv probe: client_if=0x%02X accepted the connect", found);

    return found;
}

// Registers a GATT client and connects with the interface that registration
// reported.
//
// This is the step the probe was missing. cmd 62 has no output, so the client_if
// arrives as a managed event (the queue replays old payloads, so a payload that
// differs from the replayed baseline is the new one), and - the part that
// explains the connect failure - registering is also what creates the stack-side
// connection context: the internal opcode 0x6A8 (this command) fills one of the
// five context slots that FUN_0007c0f0 looks in, and opcode 0x6AA (the connect)
// answers status 0xC8 = Bluetooth/0x1806 when it is missing (docs/ble-re.md).
static u8 pocRegisterGattClientStep(u8 previous_if)
{
    BtdrvGattAttributeUuid uuid = pocUuid16(DGLAB_COYOTE_V3_UUID16_SERVICE);
    u8 client_if = 0xFFu;
    u8 baseline[16];
    Result rc;

    memset(baseline, 0, sizeof(baseline));
    memcpy(baseline, g_ble_event.data, sizeof(baseline));

    // The registration can be refused with Bluetooth/0x14F, which FUN_00005880
    // returns when all four client slots are occupied (FUN_0000a900 counts them
    // - docs/ble-re.md). Free the interface this session's InitializeBle handed
    // out so the table has room again. Only that one is touched: it is the value
    // this process was given, never an interface read from somebody else's work.
    if (previous_if != 0xFFu) {
        rc = btdrvUnregisterGattClient(previous_if);
        pocLog("btdrv probe: UnregisterGattClient(0x%02X) rc=0x%08X", previous_if, (u32)rc);
        pocDrainBleEvents("btdrv probe after UnregisterGattClient", 500u, NULL);
    }

    rc = btdrvRegisterGattClient(&uuid);
    pocLog("btdrv probe: RegisterGattClient(0x%04X) rc=0x%08X",
        DGLAB_COYOTE_V3_UUID16_SERVICE, (u32)rc);

    for (u32 i = 0; i < 8u && !pocStopRequested(); i++) {
        BtdrvBleEventType type = (BtdrvBleEventType)0;

        memset(&g_ble_event, 0, sizeof(g_ble_event));
        rc = btdrvGetBleManagedEventInfo(&g_ble_event, sizeof(g_ble_event), &type);
        if (R_FAILED(rc)) {
            pocLog("btdrv probe: register drain rc=0x%08X", (u32)rc);
            break;
        }

        pocLog("btdrv probe: register event raw=%02X%02X%02X%02X%02X%02X%02X%02X "
               "client_if=%u",
            g_ble_event.data[0], g_ble_event.data[1], g_ble_event.data[2],
            g_ble_event.data[3], g_ble_event.data[4], g_ble_event.data[5],
            g_ble_event.data[6], g_ble_event.data[7], g_ble_event.data[4]);

        // The registration event the manager posts is 8 bytes long: the assigned
        // interface sits in byte 4 and byte 5 is 1. That marker comes straight
        // from the firmware (FUN_00005880 stores 0x0000010000000000 and then
        // overwrites byte 4 with the client_if); other events land in the same
        // queue without it, which is why "the payload changed" was not enough to
        // tell them apart (2026-09-22 round: a successful registration still
        // showed no new interface).
        if (g_ble_event.data[5] == 1u) {
            client_if = g_ble_event.data[4];
            pocLog("btdrv probe: registration event -> client_if=0x%02X", client_if);
            break;
        }
    }

    if (client_if == 0xFFu) {
        pocLog("btdrv probe: no registration event, using 0x%02X", previous_if);
        client_if = previous_if;
    }

    return client_if;
}

static void pocConnectMatrix(const char* label, u8 client_if, const BtdrvAddress* addr)
{
    Result rc;

    if (client_if == 0xFF) {
        pocLog("%s: no client_if, connect matrix skipped", label);
        return;
    }

    pocLog("%s: connect matrix for %02X:%02X:%02X:%02X:%02X:%02X (client_if=0x%02X)", label,
        addr->address[0], addr->address[1], addr->address[2], addr->address[3],
        addr->address[4], addr->address[5], client_if);

    rc = btdrvConnectGattServer(client_if, *addr, true, g_poc.aruid);
    pocLog("%s: ConnectGattServer(direct) rc=0x%08X", label, (u32)rc);
    pocDrainBleEvents(label, 1500u, NULL);

    rc = btdrvConnectGattServer(client_if, *addr, false, g_poc.aruid);
    pocLog("%s: ConnectGattServer(background) rc=0x%08X", label, (u32)rc);
    pocDrainBleEvents(label, 1500u, NULL);

    rc = btdrvTriggerConnection(*addr, 0);
    pocLog("%s: TriggerConnection(timeout 0) rc=0x%08X", label, (u32)rc);
    pocDrainBleEvents(label, 1500u, NULL);

    rc = btdrvTriggerConnection(*addr, 0x1000u);
    pocLog("%s: TriggerConnection(timeout 0x1000) rc=0x%08X", label, (u32)rc);
    pocDrainBleEvents(label, 1500u, NULL);
}

static void pocRunBtdrvScanProbe(PocWorker* w)
{
    static const u16 kInterval[4] = { 0x0060u, 0x0060u, 0x0060u, 0x0030u };
    static const u16 kWindow[4] = { 0x0030u, 0x0030u, 0x0030u, 0x0030u };
    static u8 previous_event[sizeof(((BtdrvBleEventInfo*)0)->data)];
    BtdrvAddress scanned_address;
    u8 registered_if = 0xFFu;
    Event event;
    Result rc;
    bool have_previous = false;
    u8 client_if = 0xFF;
    bool have_address = false;
    u32 total_scan_results = 0;

    // Version marker: if a log has no line below this one, the build that ran
    // is older than the counters (2026-09-21 hardware round).
    pocLog("btdrv probe: v17 (dumps the device record where it actually appears)");

    memset(&scanned_address, 0, sizeof(scanned_address));
    memset(previous_event, 0, sizeof(previous_event));

    // Cold control: one connect before anything in this session has touched BLE.
    // If it already answers Bluetooth/0x14F (the message layer's "no free task
    // slot" maps to that Result), the refusals come from the system's own BLE
    // usage rather than from anything this run did. 0xFF is never a valid
    // interface, so nothing can be disturbed by it.
    if (g_poc.use_target_address) {
        BtdrvAddress cold;

        memset(&cold, 0, sizeof(cold));
        memcpy(cold.address, g_poc.target_address, sizeof(cold.address));

        rc = btdrvInitialize();
        pocLog("btdrv probe: cold control btdrvInitialize rc=0x%08X", (u32)rc);

        if (R_SUCCEEDED(rc)) {
            rc = btdrvConnectGattServer(0xFFu, cold, true, 0);
            pocLog("btdrv probe: cold control connect client_if=0xFF rc=0x%08X", (u32)rc);
            btdrvExit();
        }
    }

    pocStopScan(w);

    // The manager registers a GATT client when InitializeBle brings it up, and
    // the connect call needs that interface number. A previous session can
    // leave the manager holding an unregistered client (ClientRegistration
    // result=0x37, client_if=0xFF), in which case a fresh btdrv session is what
    // gets a usable one - so reopen the service and try again.
    for (u32 attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            eventClose(&event);
            btdrvExit();
            svcSleepThread(500000000ull); // 500ms
        }

        rc = btdrvInitialize();
        pocLog("btdrv probe: btdrvInitialize rc=0x%08X (attempt %u)", (u32)rc, attempt);

        if (R_FAILED(rc))
            return;

        memset(&event, 0, sizeof(event));
        rc = btdrvInitializeBle(&event);
        pocLog("btdrv probe: btdrvInitializeBle rc=0x%08X", (u32)rc);

        if (R_FAILED(rc)) {
            btdrvExit();
            return;
        }

        client_if = 0xFF;
        pocDrainBleEvents("btdrv probe after InitializeBle", 1000u, &client_if);
        pocLog("btdrv probe: client_if=0x%02X (attempt %u)", client_if, attempt);

        if (client_if != 0xFF)
            break;
    }

    bool enabled = false;
    btdrvIsBluetoothEnabled(&enabled);
    pocLog("btdrv probe: adapter enabled=%u", enabled ? 1u : 0u);

    pocControlConnect("after InitializeBle");

    // Register our own GATT client now, while the BLE stack is fresh: registering
    // is what creates the stack-side connection context (internal opcode 0x6A8),
    // and a connect without it is answered with status 0xC8 = Bluetooth/0x1806
    // (docs/ble-re.md). The client_if from the event queue is not trustworthy -
    // UnregisterGattClient(0x02) proved that value is not in the manager's table
    // - so the interface this registration reports is the one to use.
    pocBtEventsOpen();
    pocBtEventsDrain("btdrv probe before register", 16u, NULL);
    pocControlConnect("after bt open");

    registered_if = pocRegisterGattClientStep(client_if);

    pocControlConnect("after RegisterGattClient");

    // The registration event is expected on the user-side channel, not on the
    // btdrv one; if it is there, it settles the interface number.
    {
        u8 from_bt = 0xFFu;

        if (pocBtEventsDrain("btdrv probe after register", 32u, &from_bt) != 0xFFu)
            registered_if = from_bt;
    }

    if (registered_if != 0xFFu && g_poc.use_target_address) {
        BtdrvAddress configured;

        memset(&configured, 0, sizeof(configured));
        memcpy(configured.address, g_poc.target_address, sizeof(configured.address));

        // The registration event did not show up, so try to find the interface it
        // created by connecting with each candidate.
        if (registered_if == client_if) {
            u8 probed = pocFindClientIfByConnect(&configured);

            if (probed != 0xFFu)
                registered_if = probed;
        }

        rc = btdrvConnectGattServer(registered_if, configured, true, 0);
        pocLog("btdrv probe: early ConnectGattServer(client_if=0x%02X) rc=0x%08X",
            registered_if, (u32)rc);
        pocDrainBleEvents("btdrv probe after early connect", 1500u, NULL);
        pocBtEventsDrain("btdrv probe after early connect", 32u, NULL);
    }

    // EnableBle comes AFTER the registration and the first connect, because the
    // 2026-09-22 round showed it is the call that exhausts the message layer:
    // a control connect answers Bluetooth/0x1806 before it and Bluetooth/0x14F
    // (the message layer's "no free task slot") right after it. The scans below
    // need it, the connect apparently must not have it first.
    rc = btdrvEnableBle();
    pocLog("btdrv probe: btdrvEnableBle rc=0x%08X", (u32)rc);
    pocControlConnect("after EnableBle");

    // Start from a known filter state: a filter left enabled by an earlier run
    // is one of the ways a scan can come back with nothing at all.
    rc = btdrvClearBleScanFilters();
    pocLog("btdrv probe: ClearBleScanFilters rc=0x%08X", (u32)rc);

    rc = btdrvEnableBleScanFilter(false);
    pocLog("btdrv probe: EnableBleScanFilter(false) rc=0x%08X", (u32)rc);

    // One phase now: the earlier four-mode run (docs/ble-poc.md) showed that
    // only a filter on the advertised manufacturer-specific data (AD 0xFF,
    // company 0x000A) makes the manager report the device - no filter and the
    // 0x180C service filter never did. The 20-second phone window is handled
    // separately after this phase.
    static const u16 kPhaseUuid[4] = { 0x0000u };
    static const u16 kPhaseCompany[4] = { POC_ADVERTISED_COMPANY_ID };
    static const bool kPhaseFilterOn[4] = { true };

    for (u32 phase = 0; phase < 1 && !pocStopRequested(); phase++) {
        u32 deadline;
        u32 fetches = 0;
        u32 empties = 0;
        u32 events = 0;
        u32 scan_results = 0;

        rc = btdrvClearBleScanFilters();
        pocLog("btdrv probe: phase %u: ClearBleScanFilters rc=0x%08X", phase, (u32)rc);

        rc = btdrvSetBleScanParameter(kInterval[phase], kWindow[phase]);
        pocLog("btdrv probe: SetBleScanParameter(0x%04X, 0x%04X) rc=0x%08X", kInterval[phase],
            kWindow[phase], (u32)rc);

        if (kPhaseUuid[phase] != 0) {
            BtdrvBleAdvertiseFilter filter;
            u16 uuid = kPhaseUuid[phase];

            memset(&filter, 0, sizeof(filter));
            filter.index = 0;
            filter.adv.size = 2;
            filter.adv.type = 0x03; // complete list of 16-bit service UUIDs
            filter.adv.data[0] = (u8)(uuid & 0xFF);
            filter.adv.data[1] = (u8)(uuid >> 8);
            filter.mask[0] = 0xFF;
            filter.mask[1] = 0xFF;
            filter.mask_size = 2;

            rc = btdrvAddBleScanFilterCondition(&filter);
            pocLog("btdrv probe: AddBleScanFilterCondition(0x%04X, type 0x03) rc=0x%08X",
                uuid, (u32)rc);
        }

        if (kPhaseCompany[phase] != 0) {
            BtdrvBleAdvertiseFilter filter;
            u16 company = kPhaseCompany[phase];

            // Manufacturer specific data: AD type 0xFF, then the company id in
            // little endian (docs/ble-poc.md).
            memset(&filter, 0, sizeof(filter));
            filter.index = 0;
            filter.adv.size = 2;
            filter.adv.type = 0xFF;
            filter.adv.data[0] = (u8)(company & 0xFF);
            filter.adv.data[1] = (u8)(company >> 8);
            filter.mask[0] = 0xFF;
            filter.mask[1] = 0xFF;
            filter.mask_size = 2;

            rc = btdrvAddBleScanFilterCondition(&filter);
            pocLog("btdrv probe: AddBleScanFilterCondition(company 0x%04X, type 0xFF) rc=0x%08X",
                company, (u32)rc);
        }

        rc = btdrvEnableBleScanFilter(kPhaseFilterOn[phase]);
        pocLog("btdrv probe: phase %u: EnableBleScanFilter(%u) rc=0x%08X", phase,
            kPhaseFilterOn[phase] ? 1u : 0u, (u32)rc);

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

            // The buffer comes back zero-filled by whoever writes the out
            // buffer, so a pre-fill pattern cannot measure the copy length
            // (2026-09-21 v9 round). Counting non-zero bytes still fingerprints
            // an event well enough.
            memset(&info, 0, sizeof(info));
            rc = btdrvGetBleManagedEventInfo(&info, sizeof(info), &type);
            fetches++;

            if (R_FAILED(rc)) {
                if (fetches % 25 == 0)
                    pocLog("btdrv probe: get event info rc=0x%08X", (u32)rc);
                continue;
            }

            // A call that found nothing leaves the (zeroed) buffer untouched.
            for (u32 i = 0; i < sizeof(info.data); i++) {
                if (info.data[i] != 0) {
                    if (first == sizeof(info.data))
                        first = i;
                    nonzero++;
                }
            }

            empty = (nonzero == 0);

            if (empty) {
                empties++;
                continue;
            }

            events++;

            bool repeat = have_previous &&
                memcmp(previous_event, info.data, sizeof(previous_event)) == 0;

            // Dump the first events and every event whose content is new: the
            // interesting payloads (a scan result among them) are the ones that
            // differ from their predecessor.
            if (events <= 3 || !repeat) {
                // Two regions, always: the head is where a ClientRegistration
                // lands, and everything device-shaped so far has sat at +0x200
                // (docs/ble-poc.md, v5-v9 rounds).
                // The first region is wider now: a scan result's AD structures
                // start at +0x0D, and the manufacturer data (AD 0xFF, company
                // 0x000A, six data bytes) is what btm's general scan needs as
                // its pattern_data.
                static const u32 kBase[2] = { 0x00u, 0x200u };
                static const u32 kRows[2] = { 6u, 2u };

                pocLog("btdrv probe: event #%u type=%u nonzero=%u first=0x%03X repeat=%u",
                    events, (u32)type, nonzero, first, repeat ? 1u : 0u);

                for (u32 region = 0; region < 2; region++) {
                    for (u32 row = 0; row < kRows[region]; row++) {
                        const u8* p = info.data + kBase[region] + row * 16u;

                        pocLog("btdrv probe:   %03X %02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X", kBase[region] + row * 16u,
                            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                            p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
                    }
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

            // libnx's type output is not usable on this firmware (it answers 0
            // or a stale value), so a scan result is recognised by its payload
            // instead: the layout is BtdrvBleScanResult and a real device has a
            // non-zero address. An all-zero address is the manager's
            // "scan started/stopped" marker, not a device.
            bool address_nonzero = false;

            for (u32 i = 0; i < sizeof(info.scan_result.address.address); i++) {
                if (info.scan_result.address.address[i] != 0)
                    address_nonzero = true;
            }

            if (!address_nonzero)
                continue;

            scan_results++;
            total_scan_results++;

            pocLog("btdrv probe: device %02X:%02X:%02X:%02X:%02X:%02X status=%u type=%u addr_type=%u entries=%u rssi=%d",
                info.scan_result.address.address[0], info.scan_result.address.address[1],
                info.scan_result.address.address[2], info.scan_result.address.address[3],
                info.scan_result.address.address[4], info.scan_result.address.address[5],
                info.scan_result.status, info.scan_result.device_type,
                info.scan_result.ble_addr_type, info.scan_result.count, info.scan_result.rssi);

            // Some events carry the address four bytes further in (after
            // result/client_if/handle fields) instead of at the BtdrvBleScanResult
            // offset, so print that reading too - the 2026-09-22 log had the
            // device at +0x0C while the libnx layout pointed at garbage.
            pocLog("btdrv probe:   (same payload at +0x0C: %02X:%02X:%02X:%02X:%02X:%02X)",
                info.data[0x0C], info.data[0x0D], info.data[0x0E], info.data[0x0F],
                info.data[0x10], info.data[0x11]);

            if (!have_address) {
                scanned_address = info.scan_result.address;
                have_address = true;
            }

        }

        pocLog("btdrv probe: phase %u done fetches=%u empty=%u events=%u scan_results=%u", phase,
            fetches, empties, events, scan_results);

        btdrvStopBleScan();
    }

    // The phone window. Keep the same kind of scan running; the moment the
    // target shows up, say so (these lines appear on the console screen, so the
    // user does not have to time anything), then keep watching for 20 seconds.
    // Whether the target's advertisements stop and come back is what separates
    // "the stack refuses" from "the device refuses" (docs/ble-poc.md).
    if (g_poc.use_target_address && !pocStopRequested()) {
        BtdrvAddress phone_target;
        BtdrvAddress seen[4];
        u32 seen_count = 0u;
        u32 scan_deadline;
        u32 window_end = 0u;
        u32 target_last_ms = 0u;
        u32 fetches = 0u;
        u32 devices = 0u;
        bool target_seen = false;
        bool advertising = false;

        memcpy(phone_target.address, g_poc.target_address, sizeof(phone_target.address));
        memset(seen, 0, sizeof(seen));

        rc = btdrvClearBleScanFilters();
        pocLog("btdrv probe: window: ClearBleScanFilters rc=0x%08X", (u32)rc);

        rc = btdrvSetBleScanParameter(0x0060u, 0x0030u);
        pocLog("btdrv probe: window: SetBleScanParameter(0x0060, 0x0030) rc=0x%08X", (u32)rc);

        {
            BtdrvBleAdvertiseFilter filter;

            memset(&filter, 0, sizeof(filter));
            filter.index = 0;
            filter.adv.size = 2;
            filter.adv.type = 0xFF;
            filter.adv.data[0] = (u8)(POC_ADVERTISED_COMPANY_ID & 0xFF);
            filter.adv.data[1] = (u8)(POC_ADVERTISED_COMPANY_ID >> 8);
            filter.mask[0] = 0xFF;
            filter.mask[1] = 0xFF;
            filter.mask_size = 2;

            rc = btdrvAddBleScanFilterCondition(&filter);
            pocLog("btdrv probe: window: AddBleScanFilterCondition(company 0x%04X) rc=0x%08X",
                (u32)POC_ADVERTISED_COMPANY_ID, (u32)rc);
        }

        rc = btdrvEnableBleScanFilter(true);
        pocLog("btdrv probe: window: EnableBleScanFilter(true) rc=0x%08X", (u32)rc);

        rc = btdrvStartBleScan();
        pocLog("btdrv probe: window: btdrvStartBleScan rc=0x%08X", (u32)rc);

        scan_deadline = pocNowMs() + POC_PROBE_FIND_MS;

        while (!pocStopRequested()) {
            BtdrvBleEventInfo info;
            BtdrvBleEventType type = 0;
            u32 now = pocNowMs();
            u32 limit = (window_end != 0u) ? window_end : scan_deadline;
            bool address_nonzero = false;
            u32 i;

            if (now >= limit)
                break;

            if (target_seen) {
                bool now_advertising = (now - target_last_ms) < 3000u;

                if (now_advertising != advertising) {
                    advertising = now_advertising;
                    pocLog("btdrv probe: window: target %s advertising (%us in)",
                        advertising ? "is" : "stopped",
                        (unsigned)((now - (window_end - POC_PROBE_PHONE_WINDOW_MS)) / 1000u));
                }
            }

            memset(&info, 0, sizeof(info));
            eventWait(&event, 200ull * 1000000ull);
            rc = btdrvGetBleManagedEventInfo(&info, sizeof(info), &type);
            fetches++;

            if (R_FAILED(rc))
                continue;

            for (i = 0; i < sizeof(info.scan_result.address.address); i++) {
                if (info.scan_result.address.address[i] != 0)
                    address_nonzero = true;
            }

            if (!address_nonzero)
                continue;

            devices++;

            {
                bool known = false;

                for (i = 0; i < seen_count; i++) {
                    if (memcmp(seen[i].address, info.scan_result.address.address, 6) == 0)
                        known = true;
                }

                if (!known && seen_count < 4u) {
                    seen[seen_count++] = info.scan_result.address;
                    pocLog("btdrv probe: window: device %02X:%02X:%02X:%02X:%02X:%02X rssi=%d",
                        info.scan_result.address.address[0], info.scan_result.address.address[1],
                        info.scan_result.address.address[2], info.scan_result.address.address[3],
                        info.scan_result.address.address[4], info.scan_result.address.address[5],
                        info.scan_result.rssi);

                    // The device record is the interesting payload and this is
                    // where it shows up: dump its AD structures (the
                    // manufacturer data at AD type 0xFF is what btm's general
                    // scan needs as pattern_data).
                    for (u32 row = 0; row < 6u; row++) {
                        const u8* p = info.data + row * 16u;

                        pocLog("btdrv probe:   %03X %02X%02X%02X%02X %02X%02X%02X%02X "
                            "%02X%02X%02X%02X %02X%02X%02X%02X", row * 16u,
                            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                            p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
                    }

                    // The docs disagree about whether the advertisement carries
                    // the service UUID 0x180C (docs/ble-poc.md), so the scan
                    // record's advertisement array is decoded here. Only for the
                    // configured device: the other devices in the room are not
                    // the point.
                    if (g_poc.use_target_address &&
                        memcmp(info.scan_result.address.address, g_poc.target_address, 6) == 0) {
                        pocLogAdArray("btdrv probe", info.scan_result.ad_list, 10u);
                    }
                }
            }

            if (memcmp(info.scan_result.address.address, phone_target.address, 6) == 0) {
                target_seen = true;
                target_last_ms = now;

                if (window_end == 0u) {
                    window_end = now + POC_PROBE_PHONE_WINDOW_MS;
                    pocLog("btdrv probe: >>> TAP CONNECT ON THE PHONE NOW <<<");
                    pocLog("btdrv probe: >>> watching the advertisement for %u more seconds <<<",
                        (unsigned)(POC_PROBE_PHONE_WINDOW_MS / 1000u));
                }
            }
        }

        btdrvStopBleScan();
        pocLog("btdrv probe: window done fetches=%u devices=%u target_seen=%u",
            fetches, devices, target_seen ? 1u : 0u);
    }

    btdrvClearBleScanFilters();
    pocLog("btdrv probe: done, %u scan result(s) in total", total_scan_results);

    pocBtEventsClose();

    // The connect method (manager +0x88 = 0x59b0) answers Bluetooth/0x14F when
    // either of its two client_if lookups finds an entry, before it ever talks
    // to the stack. So the interesting question is whether a *clean* session -
    // one where the manager has just handed out client_if - gets past that
    // check. Connect to the scanned address when the scan produced one, and to
    // the configured address otherwise.
    if (!have_address && g_poc.use_target_address) {
        memcpy(scanned_address.address, g_poc.target_address, sizeof(scanned_address.address));
        have_address = true;
        pocLog("btdrv probe: no scan result, falling back to the configured address");
    }

    if (!have_address) {
        pocLog("btdrv probe: no address to connect to (client_if=0x%02X)", client_if);
    } else {
        // Connect with the interface our own registration reported (if the
        // registration went through at all), now that the device has been seen
        // in a scan.
        if (registered_if != 0xFFu) {
            rc = btdrvConnectGattServer(registered_if, scanned_address, true, 0);
            pocLog("btdrv probe: ConnectGattServer(client_if=0x%02X, registered) rc=0x%08X",
                registered_if, (u32)rc);
            pocDrainBleEvents("btdrv probe after registered connect", 1500u, NULL);
        } else {
            pocLog("btdrv probe: no registered client_if, connect skipped");
        }

        pocConnectMatrix("btdrv probe after window", client_if, &scanned_address);
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

// The base `btm` service: the one route the sysmodule has never tried.
//
// Every earlier attempt went through btdrv directly (scans fine, the stack
// refuses the connect with status 0x68 / Bluetooth/0x1806) or through btm:u,
// which is applet-only (Sf/0x60A when a real applet ARUID is passed). The base
// `btm` service is a third thing: the identity probe has read btmGetState
// through it since the first hardware round, so this process is accepted, and
// it carries the whole BLE surface the applet path uses - scans, connect,
// connection state, GATT - from a service that is not restricted to applets.
//
// This is why the probe runs from a session started with
// DGLAB_POC_START_FLAG_SKIP_PROBES and before any bt/btm:u/btdrv call of its
// own: the BLE manager binds its state to the session that initialized it
// (docs/ble-poc.md), so everything below has to be the first BLE access of the
// run for its result to mean anything.
//
// HAZARD - read this before adding anything here.
//
// The 2026-09-22 round ended with the *btm* module aborting (svcBreak through
// its own terminate path; the crash report is in the user's downloads) right
// after the NRO exited, taking hid down with it. That round was the one that
// also brought btdrv's BLE up in the same session while btm had work queued:
// every btm call afterwards answered 0x668F, and btm's worker was still holding
// the connect request for this device when it died. The same round registered
// the applet's ARUID, which stops being valid when the applet exits.
//
// So this probe stays on btm's own read-only surface: state, stored scan
// parameters, two short scans, the client-condition dump. No ARUID adoption, no
// btdrv BLE bring-up, no connect unless a scan actually reported the device, and
// no radio/pairing/audio/gamepad commands. Anything beyond that needs the
// btm/btdrv ownership rules worked out statically first - see docs/ble-re.md.
//
// Status (2026-09-22, docs/ble-re.md "当前总览"): the btdrv-direct route is
// blocked by resource ownership (btm holds the four client slots; EnableBle is
// required to register but is also what makes the message layer refuse connect
// requests), and btm's route is accepted but the stack's connect attempt comes
// back as "not established" (status=2, no conn_id, no reason). The probe now
// runs only when the user asks for it (StickR or idle-B): it leaves btm holding
// a request, so it must not run unattended after every boot.

// Walks the advertisement structures (length / type / value) of a scan record.
//
// Which offset the AD structures start at is not the same in every record on
// this firmware: the libnx BtdrvBleScanResult layout puts the address at +1,
// while the 2026-09-22 logs had the device at +0x0C. So instead of trusting one
// offset, every plausible start is tried and the first one that parses as a
// chain of at least three AD structures - one of them a known type - is
// printed. That is what settles "does the advertisement carry the service UUID
// 0x180C" with data instead of with a note in the docs.
static void pocLogAdStructures(const char* label, const u8* data, size_t size)
{
    for (size_t start = 0; start <= 0x40u && start + 6u <= size; start++) {
        size_t offset = start;
        u32 entries = 0;
        bool known = false;
        bool ok = true;

        while (offset + 1u < size) {
            u8 len = data[offset];

            if (len == 0)
                break;

            if (len > 0x1Fu || offset + 2u + len > size) {
                ok = false;
                break;
            }

            switch (data[offset + 1u]) {
                case 0x01: case 0x02: case 0x03: case 0x06: case 0x07: case 0x08:
                case 0x09: case 0x0A: case 0x0D: case 0x0E: case 0x0F: case 0x10:
                case 0xFF:
                    known = true;
                    break;
                default:
                    break;
            }

            entries++;
            offset += 2u + len;
        }

        if (!ok || entries < 3u || !known)
            continue;

        pocLog("%s AD structures at +0x%02X", label, (unsigned)start);

        for (offset = start; offset + 1u < size;) {
            char value[3u * 0x1Fu + 1u];
            u8 len = data[offset];

            if (len == 0)
                break;

            pocHex(value, sizeof(value), data + offset + 2u, len);
            pocLog("%s   type=0x%02X len=%u %s", label, data[offset + 1u], (unsigned)len,
                value);
            offset += 2u + len;
        }

        return;
    }

    pocLog("%s no AD structure chain in the first 0x40 bytes", label);
}

// A scan record carries the advertisement as a fixed array of ten entries
// (`BtdrvBleAdvertisement`: size / type / data), not as one packed chain. The
// chain walker above therefore reported "no AD structure chain" for a record
// that has three entries in it (2026-09-25 runs: flags, manufacturer data with
// company 0x000A, and the local name). Decoding the array is what settles
// whether this device advertises a service UUID at all.
static void pocLogAdArray(const char* label, const BtdrvBleAdvertisement* list, u32 count)
{
    u32 entries = 0;

    for (u32 i = 0; i < count; i++) {
        if (list[i].size == 0u || list[i].size > (1u + sizeof(list[i].data)))
            break;

        entries++;
    }

    pocLog("%s advertisement: %u entry(ies)", label, (unsigned)entries);

    for (u32 i = 0; i < entries; i++) {
        char value[3u * sizeof(list[i].data) + 1u];

        pocHex(value, sizeof(value), list[i].data, (size_t)list[i].size - 1u);
        pocLog("%s   ad[%u] type=0x%02X len=%u %s", label, i, list[i].type,
            (unsigned)list[i].size, value);
    }
}

// One btm scan pass. smart_device picks the smart-device scan, which is the
// UUID-filtered one Nintendo's own flow uses; otherwise the manufacturer-data
// filter is used. Returns true when the interesting device was reported.
//
// btm signals new results through its scan event, so every poll waits on that
// event first and only then reads the result list - polling without draining the
// event is what an empty result set looks like (2026-09-22, applet probe).
//
// `suffix` goes into every log line so two passes of the same kind stay apart in
// the log ("" for the first, " (BLE up)" for the second).
static bool pocBtmScanPass(const char* suffix, bool smart_device, Event* scan_event,
    bool have_event, BtdrvAddress* out)
{
    BtdrvGattAttributeUuid uuid = pocUuid16(POC_UUID16_ADVERTISED_SERVICE);
    char label[64];
    Result rc;
    bool found = false;
    u32 devices = 0;
    u32 events = 0;

    snprintf(label, sizeof(label), "btm %s scan%s", smart_device ? "smart" : "general", suffix);

    // btm answers 0x668F ("no free task slot") while the BLE thread is still
    // settling right after the stack was turned on, so the start is retried.
    rc = 0;

    for (u32 attempt = 0; attempt < 3u && !pocStopRequested() && rc != 0; attempt++) {
        if (attempt > 0u)
            svcSleepThread(1000000000ull); // 1s

        if (smart_device) {
            rc = btmStartBleScanForSmartDevice(&uuid);
            pocLog("%s: StartBleScanForSmartDevice(0x%04X) rc=0x%08X (attempt %u)", label,
                POC_UUID16_ADVERTISED_SERVICE, (u32)rc, attempt);
        } else {
            BtdrvBleAdvertisePacketParameter param;

            memset(&param, 0, sizeof(param));
            param.company_id = POC_ADVERTISED_COMPANY_ID;
            rc = btmStartBleScanForGeneral(param);
            pocLog("%s: StartBleScanForGeneral(company=0x%04X) rc=0x%08X (attempt %u)", label,
                POC_ADVERTISED_COMPANY_ID, (u32)rc, attempt);
        }
    }

    if (R_FAILED(rc))
        return false;

    for (u32 poll = 0; poll < 12u && !pocStopRequested() && !found; poll++) {
        u8 total = 0;

        if (have_event && R_SUCCEEDED(eventWait(scan_event, 500000000ull))) { // 500ms
            events++;
            pocLog("%s: scan event #%u after poll %u", label, events, poll);
        }

        memset(g_btm_scan_results, 0, sizeof(g_btm_scan_results));

        if (smart_device)
            rc = btmGetBleScanResultsForSmartDevice(g_btm_scan_results, POC_BTM_SCAN_MAX, &total);
        else
            rc = btmGetBleScanResultsForGeneral(g_btm_scan_results, POC_BTM_SCAN_MAX, &total);

        if (poll < 2u || (R_SUCCEEDED(rc) && total > 0u))
            pocLog("%s: poll %u rc=0x%08X total=%u", label, poll, (u32)rc, (unsigned)total);

        if (R_SUCCEEDED(rc) && total > 0u) {
            for (u32 k = 0; k < total && k < POC_BTM_SCAN_MAX; k++) {
                const u8* record = (const u8*)&g_btm_scan_results[k];
                BtdrvAddress reported;
                bool nonzero = false;
                bool match;

                memset(&reported, 0, sizeof(reported));
                memcpy(reported.address, record + 1u, sizeof(reported.address));

                for (u32 i = 0; i < sizeof(reported.address); i++) {
                    if (reported.address[i] != 0)
                        nonzero = true;
                }

                if (!nonzero)
                    continue;

                devices++;
                match = g_poc.use_target_address &&
                    memcmp(reported.address, g_poc.target_address, sizeof(reported.address)) == 0;

                // Two address readings: libnx's BtdrvBleScanResult puts the
                // address at +1, while the 2026-09-22 btdrv logs had the device
                // at +0x0C. Print both instead of picking one.
                pocLog("%s: dev[%u] ^0x01=%02X:%02X:%02X:%02X:%02X:%02X "
                       "^0x0C=%02X:%02X:%02X:%02X:%02X:%02X",
                    label, k,
                    reported.address[0], reported.address[1], reported.address[2],
                    reported.address[3], reported.address[4], reported.address[5],
                    record[0x0Cu], record[0x0Du], record[0x0Eu],
                    record[0x0Fu], record[0x10u], record[0x11u]);

                // A device is only interesting for the connect when it is the
                // configured one; without a configured address the first device
                // btm reports is used, so a console without the config file can
                // still be exercised.
                if (!g_poc.use_target_address || match) {
                    pocLog("%s:   %s record head %02X%02X%02X%02X%02X%02X%02X%02X "
                           "%02X%02X%02X%02X%02X%02X%02X%02X",
                        label, g_poc.use_target_address ? "target" : "first",
                        record[0], record[1], record[2], record[3], record[4], record[5],
                        record[6], record[7], record[8], record[9], record[10], record[11],
                        record[12], record[13], record[14], record[15]);
                    pocLogAdStructures(label, record, sizeof(g_btm_scan_results[k]));

                    memcpy(out->address, reported.address, sizeof(out->address));
                    found = true;
                    break;
                }
            }
        }

        if (!found)
            svcSleepThread(500000000ull); // 500ms
    }

    if (smart_device)
        rc = btmStopBleScanForSmartDevice();
    else
        rc = btmStopBleScanForGeneral();

    pocLog("%s: stop rc=0x%08X devices=%u events=%u found=%u", label, (u32)rc, devices, events,
        found ? 1u : 0u);
    return found;
}

// Dumps the connection state btm keeps. Read-only, and only when a connect was
// refused: the list is what says whether btm has any client registered for this
// process at all.
static void pocBtmLogConnectionState(const char* label)
{
    u8 total = 0;
    Result rc;

    memset(g_btm_connections, 0, sizeof(g_btm_connections));
    rc = btmBleGetConnectionState(g_btm_connections, 4u, &total);
    pocLog("%s: GetConnectionState rc=0x%08X total=%u", label, (u32)rc, (unsigned)total);

    for (u32 k = 0; k < total && k < 4u; k++) {
        pocLog("%s:   state[%u] handle=%u addr=%02X:%02X:%02X:%02X:%02X:%02X", label, k,
            g_btm_connections[k].connection_handle,
            g_btm_connections[k].addr.address[0], g_btm_connections[k].addr.address[1],
            g_btm_connections[k].addr.address[2], g_btm_connections[k].addr.address[3],
            g_btm_connections[k].addr.address[4], g_btm_connections[k].addr.address[5]);
    }
}

// btm's GATT client bookkeeping, raw. libnx does not decode the 0x74 bytes; the
// interesting part is whether the entry btm keeps for this caller is empty.
//
// Printed in 0x28-byte chunks: the whole 0x74 bytes on one line is 232 hex
// characters and the log ring truncates around 160, which is how the 2026-09-22
// dump ended up cut in half.
static void pocBtmLogClientCondition(const char* label)
{
    BtmGattClientConditionList list;
    Result rc;

    memset(&list, 0, sizeof(list));
    rc = btmBleGetGattClientConditionList(&list);
    pocLog("%s: GetGattClientConditionList rc=0x%08X", label, (u32)rc);

    for (size_t offset = 0; offset < sizeof(list.unk_x0); offset += 0x28u) {
        size_t chunk = sizeof(list.unk_x0) - offset;
        char hex[3u * 0x28u + 1u];

        if (chunk > 0x28u)
            chunk = 0x28u;

        pocHex(hex, sizeof(hex), list.unk_x0 + offset, chunk);
        pocLog("%s:   %02X %s", label, (unsigned)offset, hex);
    }
}

static bool pocBtmConnectTry(const char* label, const BtdrvAddress* addr, u32* out_handle);

// Looks for a fresh "connected" event on the bt channel for this device.
//
// btmBleGetConnectionState stays at total=0 in this setup, so the connection
// state that the stack actually reports has to come from the user-side events:
// a client_connection record with status 0 (connected), a valid conn_id and our
// address. Older records (a previous session's disconnect, for example) have
// status 2 and are ignored. Returns the connection id, or 0xFFFFFFFF.
static u32 pocBtFindConnectionEvent(const BtdrvAddress* addr, u32 timeout_ms)
{
    u32 deadline = pocNowMs() + timeout_ms;
    u32 conn_id = 0xFFFFFFFFu;
    // The queue replays the same record over and over; logging every repeat
    // filled the log ring (5760 identical lines in the 2026-09-25 run). Only a
    // change is worth a line.
    static u32 last_status = 0xFFu;
    static u32 last_conn_id = 0xFFFFFFFFu;
    static u32 last_reason = 0xFFFFu;

    while ((s32)(deadline - pocNowMs()) > 0 && !pocStopRequested()) {
        for (u32 i = 0; i < 16u; i++) {
            BtdrvBleEventType type = (BtdrvBleEventType)0;
            Result rc;
            bool zero = true;

            memset(&g_ble_event, 0, sizeof(g_ble_event));
            rc = btGetLeEventInfo(&g_ble_event, sizeof(g_ble_event), &type);

            if (R_FAILED(rc))
                break;

            for (u32 b = 0; b < 8u; b++) {
                if (g_ble_event.data[b] != 0)
                    zero = false;
            }

            if (zero)
                break;

            if (g_ble_event.client_connection.conn_id == 0xFFFFFFFFu ||
                memcmp(g_ble_event.client_connection.address.address, addr->address, 6) != 0)
                continue;

            if (g_ble_event.client_connection.status != last_status ||
                g_ble_event.client_connection.conn_id != last_conn_id ||
                g_ble_event.client_connection.reason != last_reason) {
                last_status = g_ble_event.client_connection.status;
                last_conn_id = g_ble_event.client_connection.conn_id;
                last_reason = g_ble_event.client_connection.reason;

                pocLog("%s: bt connection event status=%u conn_id=%u reason=0x%04X", "btm probe",
                    last_status, last_conn_id, last_reason);
            }

            if (g_ble_event.client_connection.status == 0u) {
                conn_id = g_ble_event.client_connection.conn_id;
                break;
            }
        }

        if (conn_id != 0xFFFFFFFFu)
            break;

        svcSleepThread(50000000ull); // 50ms
    }

    return conn_id;
}

// Connect with a few retries: btm/the stack answer 0x668F ("no free task slot")
// for a moment after the BLE stack was turned on, and a refused attempt leaves
// nothing behind, so retrying is safe.
static bool pocBtmConnectRetry(const char* label, const BtdrvAddress* addr, u32* out_handle)
{
    for (u32 attempt = 0; attempt < 3u && !pocStopRequested(); attempt++) {
        if (attempt > 0u) {
            pocLog("%s: retrying the connect in 2s (attempt %u)", label, attempt);
            svcSleepThread(2000000000ull); // 2s
        }

        if (pocBtmConnectTry(label, addr, out_handle))
            return true;
    }

    return false;
}

// One connect attempt through btm: queue the connect, then wait on the connection
// event and poll the connection list. Returns true and writes the handle when a
// connection showed up.
static bool pocBtmConnectTry(const char* label, const BtdrvAddress* addr, u32* out_handle)
{
    Event conn_event;
    bool have_event = false;
    u32 handle = 0xFFFFFFFFu;
    Result rc;

    memset(&conn_event, 0, sizeof(conn_event));

    rc = btmBleConnect(*addr);
    pocLog("%s: BleConnect(%02X:%02X:%02X:%02X:%02X:%02X) rc=0x%08X", label,
        addr->address[0], addr->address[1], addr->address[2], addr->address[3],
        addr->address[4], addr->address[5], (u32)rc);

    if (R_FAILED(rc))
        return false;

    rc = btmAcquireBleConnectionEvent(&conn_event);
    have_event = R_SUCCEEDED(rc);
    pocLog("%s: AcquireBleConnectionEvent rc=0x%08X", label, (u32)rc);

    if (have_event) {
        u32 deadline = pocNowMs() + POC_CONNECT_TIMEOUT_MS;
        u32 events = 0;
        u32 logged = 0;

        while ((s32)(deadline - pocNowMs()) > 0 && !pocStopRequested()) {
            // Primary signal: the user-side channel reports the connection the
            // stack really made (conn_id + address). btm's own connection list
            // stays empty in this setup, so this is what says "connected".
            handle = pocBtFindConnectionEvent(addr, 250u);

            if (handle != 0xFFFFFFFFu) {
                pocLog("%s: connected (bt event) conn_id=%u addr=%02X:%02X:%02X:%02X:%02X:%02X",
                    label, handle, addr->address[0], addr->address[1], addr->address[2],
                    addr->address[3], addr->address[4], addr->address[5]);
                break;
            }

            if (R_SUCCEEDED(eventWait(&conn_event, 250000000ull))) { // 250ms
                events++;

                if (logged < 4u) {
                    logged++;
                    pocLog("%s: connection state event #%u", label, events);
                }
            }

            {
                u8 total = 0;
                Result state_rc;

                memset(g_btm_connections, 0, sizeof(g_btm_connections));
                state_rc = btmBleGetConnectionState(g_btm_connections, 4u, &total);

                if (R_SUCCEEDED(state_rc) && total > 0u) {
                    handle = g_btm_connections[0].connection_handle;
                    pocLog("%s: connected handle=%u addr=%02X:%02X:%02X:%02X:%02X:%02X "
                           "(events=%u)",
                        label, handle, g_btm_connections[0].addr.address[0],
                        g_btm_connections[0].addr.address[1],
                        g_btm_connections[0].addr.address[2],
                        g_btm_connections[0].addr.address[3],
                        g_btm_connections[0].addr.address[4],
                        g_btm_connections[0].addr.address[5], events);
                    break;
                }
            }
        }
    }

    if (have_event)
        eventClose(&conn_event);

    if (handle == 0xFFFFFFFFu) {
        pocLog("%s: no connection", label);
        return false;
    }

    *out_handle = handle;
    return true;
}

// libnx has no decoded descriptor entry for btm, so each one is printed with its
// raw bytes and the 0x2902 (client characteristic configuration) candidate is
// kept for the read-back in pocBtmTransportStart. The instance id is taken from
// +0x1C - where it sits in both the service and characteristic structs - and the
// dump is what shows whether the guess holds.
static void pocBtmLogDescriptors(const char* label, u32 handle, u16 char_handle)
{
    BtmGattDescriptor descs[8];
    u8 total = 0;
    Result rc;

    memset(descs, 0, sizeof(descs));
    rc = btmGetGattDescriptors(handle, char_handle, descs, 8u, &total);
    pocLog("%s: GetGattDescriptors(char %u) rc=0x%08X total=%u", label,
        (unsigned)char_handle, (u32)rc, (unsigned)total);

    for (u32 i = 0; i < total && i < 8u; i++) {
        const u8* raw = (const u8*)&descs[i];
        u16 candidate_id = (u16)(raw[0x1C] | (raw[0x1D] << 8));
        char text[64];
        char raw_label[64];

        pocUuidText(text, sizeof(text), &descs[i].uuid);
        pocLog("%s:     desc[%u] uuid=%s handle=%u id?=%u", label, i, text,
            descs[i].handle, (unsigned)candidate_id);

        snprintf(raw_label, sizeof(raw_label), "%s:     desc[%u] raw", label, i);
        pocLogWords(raw_label, raw, sizeof(BtmGattDescriptor), 0x14u);

        if (descs[i].uuid.size == 2 && descs[i].uuid.uuid[0] == 0x02u &&
            descs[i].uuid.uuid[1] == 0x29u) {
            memset(&g_btm_cccd, 0, sizeof(g_btm_cccd));
            g_btm_cccd.instance_id = (u8)candidate_id;
            g_btm_cccd.uuid = descs[i].uuid;
            g_btm_cccd_ready = true;
        }
    }
}

// Prints the GATT table of a connection and sets the same milestones the btdev
// session uses, so the NRO's status line reads the same way.
static void pocBtmLogGatt(const char* label, u32 handle)
{
    u8 total = 0;
    Result rc;

    pocSetMilestone(DGLAB_POC_MILESTONE_CONNECTED);

    // Service discovery runs after the connection is up, so the first query can
    // legitimately answer "no services yet" (the 2026-09-24 run did exactly
    // that: it connected, asked once immediately and got total=0). Wait for the
    // service-discovery event and retry for a few seconds.
    {
        Event discovery_event;
        bool have_event = false;
        u32 deadline = pocNowMs() + POC_DISCOVER_TIMEOUT_MS;

        memset(&discovery_event, 0, sizeof(discovery_event));
        rc = btmAcquireBleServiceDiscoveryEvent(&discovery_event);
        have_event = R_SUCCEEDED(rc);
        pocLog("%s: AcquireBleServiceDiscoveryEvent rc=0x%08X", label, (u32)rc);

        for (u32 attempt = 0; attempt < 8u && !pocStopRequested(); attempt++) {
            if (have_event && R_SUCCEEDED(eventWait(&discovery_event, 1000000000ull)))
                pocLog("%s: service discovery event after %u attempt(s)", label, attempt);

            memset(g_btm_services, 0, sizeof(g_btm_services));
            total = 0;
            rc = btmGetGattServices(handle, g_btm_services, POC_BTM_SERVICE_MAX, &total);
            pocLog("%s: GetGattServices attempt %u rc=0x%08X total=%u", label, attempt,
                (u32)rc, (unsigned)total);

            if (R_SUCCEEDED(rc) && total > 0u)
                break;

            if ((s32)(deadline - pocNowMs()) < 0)
                break;

            svcSleepThread(500000000ull); // 500ms
        }

        if (have_event)
            eventClose(&discovery_event);
    }

    for (u32 i = 0; i < total && i < POC_BTM_SERVICE_MAX; i++) {
        char text[64];

        pocUuidText(text, sizeof(text), &g_btm_services[i].uuid);
        pocLog("%s:   service[%u] uuid=%s handle=%u end=%u primary=%u", label, i, text,
            g_btm_services[i].handle, g_btm_services[i].end_group_handle,
            g_btm_services[i].primary_service ? 1u : 0u);

        if (g_btm_services[i].uuid.size == 2 &&
            g_btm_services[i].uuid.uuid[0] == (u8)(DGLAB_COYOTE_V3_UUID16_SERVICE & 0xFF) &&
            g_btm_services[i].uuid.uuid[1] == (u8)(DGLAB_COYOTE_V3_UUID16_SERVICE >> 8)) {
            u8 chars = 0;
            char raw_label[64];

            pocSetMilestone(DGLAB_POC_MILESTONE_SERVICE_FOUND);

            snprintf(raw_label, sizeof(raw_label), "%s:   service[%u] raw", label, i);
            pocLogWords(raw_label, (const u8*)&g_btm_services[i], sizeof(BtmGattService), 0u);

            memset(g_btm_characteristics, 0, sizeof(g_btm_characteristics));
            rc = btmGetGattCharacteristics(handle, g_btm_services[i].handle,
                g_btm_characteristics, POC_BTM_CHARACTERISTIC_MAX, &chars);
            pocLog("%s: GetGattCharacteristics rc=0x%08X total=%u", label, (u32)rc,
                (unsigned)chars);

            for (u32 c = 0; c < chars && c < POC_BTM_CHARACTERISTIC_MAX; c++) {
                pocUuidText(text, sizeof(text), &g_btm_characteristics[c].uuid);
                pocLog("%s:     char[%u] uuid=%s handle=%u props=0x%02X", label, c, text,
                    g_btm_characteristics[c].handle,
                    g_btm_characteristics[c].properties);

                // The protocol characteristics are the ones the transport uses,
                // so their whole struct is dumped: handle at +0x18, instance_id
                // at +0x1C, properties at +0x1E in libnx's layout.
                snprintf(raw_label, sizeof(raw_label), "%s:     char[%u] raw", label, c);
                pocLogWords(raw_label, (const u8*)&g_btm_characteristics[c],
                    sizeof(BtmGattCharacteristic), 0x14u);

                // Remember the two characteristics the transport writes to and
                // listens on, so the connection can be driven afterwards.
                if (g_btm_characteristics[c].uuid.size == 2 &&
                    g_btm_characteristics[c].uuid.uuid[0] ==
                        (u8)(DGLAB_COYOTE_V3_UUID16_CHAR_WRITE & 0xFF) &&
                    g_btm_characteristics[c].uuid.uuid[1] ==
                        (u8)(DGLAB_COYOTE_V3_UUID16_CHAR_WRITE >> 8)) {
                    g_btm_proto_service = g_btm_services[i];
                    g_btm_proto_write = g_btm_characteristics[c];
                } else if (g_btm_characteristics[c].uuid.size == 2 &&
                    g_btm_characteristics[c].uuid.uuid[0] ==
                        (u8)(DGLAB_COYOTE_V3_UUID16_CHAR_NOTIFY & 0xFF) &&
                    g_btm_characteristics[c].uuid.uuid[1] ==
                        (u8)(DGLAB_COYOTE_V3_UUID16_CHAR_NOTIFY >> 8)) {
                    g_btm_proto_service = g_btm_services[i];
                    g_btm_proto_notify = g_btm_characteristics[c];

                    // The CCCD lives under this characteristic; what is written
                    // there is what RegisterNotification has to leave behind.
                    pocBtmLogDescriptors(label, handle, g_btm_characteristics[c].handle);
                }
            }
        } else if (g_btm_services[i].uuid.size == 2 &&
            g_btm_services[i].uuid.uuid[0] ==
                (u8)(DGLAB_COYOTE_V3_UUID16_BATTERY_SERVICE & 0xFF) &&
            g_btm_services[i].uuid.uuid[1] ==
                (u8)(DGLAB_COYOTE_V3_UUID16_BATTERY_SERVICE >> 8)) {
            // The battery characteristic is a plain read: it exercises the GATT
            // client + event path without driving any output, which is exactly
            // what the notification investigation needs.
            u8 chars = 0;

            memset(g_btm_characteristics, 0, sizeof(g_btm_characteristics));
            rc = btmGetGattCharacteristics(handle, g_btm_services[i].handle,
                g_btm_characteristics, POC_BTM_CHARACTERISTIC_MAX, &chars);
            pocLog("%s: battery GetGattCharacteristics rc=0x%08X total=%u", label, (u32)rc,
                (unsigned)chars);

            for (u32 c = 0; c < chars && c < POC_BTM_CHARACTERISTIC_MAX; c++) {
                char raw_label[64];

                pocUuidText(text, sizeof(text), &g_btm_characteristics[c].uuid);
                pocLog("%s:   battery char[%u] uuid=%s handle=%u props=0x%02X", label, c, text,
                    g_btm_characteristics[c].handle, g_btm_characteristics[c].properties);

                // Same tail dump as the protocol characteristics: all eight
                // property bytes coming back 0x00 is what sent the transport
                // looking for the field that actually carries them.
                snprintf(raw_label, sizeof(raw_label), "%s:   battery char[%u] raw", label, c);
                pocLogWords(raw_label, (const u8*)&g_btm_characteristics[c],
                    sizeof(BtmGattCharacteristic), 0x14u);

                if (g_btm_characteristics[c].uuid.size == 2 &&
                    g_btm_characteristics[c].uuid.uuid[0] ==
                        (u8)(DGLAB_COYOTE_V3_UUID16_CHAR_BATTERY & 0xFF) &&
                    g_btm_characteristics[c].uuid.uuid[1] ==
                        (u8)(DGLAB_COYOTE_V3_UUID16_CHAR_BATTERY >> 8)) {
                    g_btm_battery_service = g_btm_services[i];
                    g_btm_battery_char = g_btm_characteristics[c];
                    g_btm_battery_ready = true;
                }
            }
        }
    }

    g_btm_proto_ready = g_btm_proto_service.uuid.size == 2 &&
        g_btm_proto_write.uuid.size == 2 && g_btm_proto_notify.uuid.size == 2;

    if (g_btm_proto_ready)
        pocLog("%s: protocol coordinates found (service handle=%u, write=%u, notify=%u)",
            label, g_btm_proto_service.handle, g_btm_proto_write.handle,
            g_btm_proto_notify.handle);
}

static void pocRunBtmBleProbe(PocWorker* w)
{
    BtdrvAddress address;
    Event scan_event;
    bool have_scan_event = false;
    bool have_address = false;
    u32 handle = 0xFFFFFFFFu;
    Result rc;

    (void)w;

    memset(&address, 0, sizeof(address));
    memset(&scan_event, 0, sizeof(scan_event));

    // This probe deliberately does NOT touch btdrv at all.
    //
    // Two attempts to make the probe self-sufficient both broke the connect:
    //   - calling btdrvInitializeBle + btdrvEnableBle here (a second stack
    //     initialisation in a boot the driver-level probe already prepared), and
    //   - calling RegisterGattClient here (btm then drove the connect through
    //     the client context this process created, client_if=3, which never
    //     establishes a link).
    // Every run that did either failed; the two runs that connected
    // (2026-09-24 22:17/22:31) had a btm probe that only used the btm/bt
    // services, with the driver-level probe having run in an earlier session.
    // So the recipe is: press D-pad Left first, then StickR / idle-B.
    pocLog("btm probe: base btm only; run the D-pad Left probe first if the BLE "
           "stack has not been brought up in this boot");

    pocLog("=== btm probe: base btm service, sysmodule calling ===");

    rc = btmInitialize();
    pocLog("btm probe: btmInitialize rc=0x%08X", (u32)rc);
    if (R_FAILED(rc))
        return;

    {
        BtmState state = BtmState_NotInitialized;
        Result state_rc = btmGetState(&state);

        pocLog("btm probe: GetState rc=0x%08X state=%u", (u32)state_rc, (u32)state);
    }

    // Self-validating baseline. These two reads are btm's stored scan
    // parameters; a wrong command number cannot produce a company id and an
    // UUID that match what the console has configured, so they say whether
    // libnx's btm bindings are live on this firmware at all before a scan is
    // blamed.
    {
        BtdrvBleAdvertisePacketParameter param;
        BtdrvGattAttributeUuid uuid;
        char text[64];

        memset(&param, 0, sizeof(param));
        rc = btmGetBleScanParameterGeneral(0xFFFFu, &param);
        pocLog("btm probe: GetBleScanParameterGeneral(0xFFFF) rc=0x%08X company=0x%04X "
               "pattern=%02X%02X%02X%02X%02X%02X",
            (u32)rc, param.company_id, param.pattern_data[0], param.pattern_data[1],
            param.pattern_data[2], param.pattern_data[3], param.pattern_data[4],
            param.pattern_data[5]);

        memset(&uuid, 0, sizeof(uuid));
        rc = btmGetBleScanParameterSmartDevice(0x2u, &uuid);
        pocUuidText(text, sizeof(text), &uuid);
        pocLog("btm probe: GetBleScanParameterSmartDevice(2) rc=0x%08X size=%u uuid=%s",
            (u32)rc, (unsigned)uuid.size, text);
    }

    // Registering the NRO's ARUID with btm is deliberately NOT done any more.
    //
    // It was the first hypothesis (btm identifies its BLE clients by ARUID, and
    // the base service has no ARUID field in its requests, only this command).
    // The 2026-09-22 hardware round showed it is accepted - and that btm still
    // does nothing afterwards. Since btm may keep referring to that identity, and
    // the crash that followed happened right after the applet (whose ARUID this
    // was) exited, the sysmodule no longer adopts an applet identity at all.
    pocLog("btm probe: ARUID registration skipped (applet identity is not adopted)");

    rc = btmAcquireBleScanEvent(&scan_event);
    have_scan_event = R_SUCCEEDED(rc);
    pocLog("btm probe: AcquireBleScanEvent rc=0x%08X", (u32)rc);

    // Phase A: nothing in this session has touched Bluetooth yet.
    if (pocBtmScanPass("", false, &scan_event, have_scan_event, &address)) {
        have_address = true;
        pocSetMilestone(DGLAB_POC_MILESTONE_DEVICE_FOUND);
    } else {
        pocLog("btm probe: phase A general scan reported no device");
    }

    // Control pass: the UUID-filtered scan is the one Nintendo's smart-device
    // flow uses, and whether it can see this device is the open 0x180C question.
    {
        BtdrvAddress smart_address;

        memset(&smart_address, 0, sizeof(smart_address));

        if (!pocBtmScanPass(" (smart)", true, &scan_event, have_scan_event, &smart_address)) {
            pocLog("btm probe: phase A smart scan reported no device");
        } else if (!have_address) {
            memcpy(address.address, smart_address.address, sizeof(address.address));
            have_address = true;
            pocSetMilestone(DGLAB_POC_MILESTONE_DEVICE_FOUND);
        }
    }

    // Paired / auto-connection pass.
    //
    // This is the last precondition of the Nintendo-shaped flow that had never
    // been tried: btm's own "scan for paired devices" is what arms auto
    // connection for devices the console already knows (libnx wraps the btm:u
    // form as btdevEnableBleAutoConnection). If the stack wants a known device
    // before it establishes a link, this is where that shows up. The paired scan
    // has no result list in libnx - it is the auto-connection path.
    {
        BtdrvBleAdvertisePacketParameter param;
        u8 total = 0;

        memset(&param, 0, sizeof(param));
        param.company_id = POC_ADVERTISED_COMPANY_ID;

        rc = btmStartBleScanForPaired(param);
        pocLog("btm probe: StartBleScanForPaired(company=0x%04X) rc=0x%08X",
            POC_ADVERTISED_COMPANY_ID, (u32)rc);

        for (u32 i = 0; i < 6u && !pocStopRequested(); i++) {
            pocBtEventsDrain("btm probe paired scan", 8u, NULL);
            svcSleepThread(500000000ull); // 500ms
        }

        rc = btmStopBleScanForPaired();
        pocLog("btm probe: StopBleScanForPaired rc=0x%08X", (u32)rc);

        memset(g_btm_connections, 0, sizeof(g_btm_connections));
        rc = btmBleGetConnectionState(g_btm_connections, 4u, &total);
        pocLog("btm probe: connection state after paired scan rc=0x%08X total=%u", (u32)rc,
            (unsigned)total);
    }

    // Connect only when a scan actually reported the device.
    //
    // It used to connect to the configured address even on a scan miss, copying
    // what the applet probe does. That request is exactly what was still sitting
    // in btm's worker when btm aborted on 2026-09-22 - the crash report's thread
    // stack held this device address - so it is gone: a blind connect has no
    // diagnostic value left (btm accepts it and does nothing) and it leaves btm
    // holding work it cannot complete.
    //
    // The `bt` service is opened for this part on purpose: btm is the only
    // client on this console with a connection context (btm's own registration
    // fills the client slots - docs/ble-re.md), its connect is accepted, and the
    // user-side channel is where an applet sees what the stack does with it. The
    // btdrv side is deliberately NOT brought up here: doing that while btm has
    // work in flight is the combination that aborted btm on 2026-09-22.
    pocBtEventsOpen();
    pocBtEventsDrain("btm probe before connect", 16u, NULL);

    if (have_address) {
        if (pocBtmConnectRetry("btm probe", &address, &handle)) {
            pocBtmLogGatt("btm probe", handle);

            // With the GATT table in hand, drive the device through the protocol
            // layer for a few seconds: BF + a B0 stream (strength 0) and the B1
            // answers, which is what a real transport has to do.
            if (g_btm_proto_ready) {
                pocBtmTransportStart(handle);
                pocBtmTransportPump(3000u);
                pocBtmTransportStop();
            } else {
                pocLog("btm probe: protocol coordinates missing, transport not started");
            }

            rc = btmBleDisconnect(handle);
            pocLog("btm probe: BleDisconnect rc=0x%08X", (u32)rc);
        } else {
            pocBtmLogConnectionState("btm probe");
            pocBtmLogClientCondition("btm probe");
        }
    } else if (g_poc.use_target_address) {
        // The configured address is the one thing worth connecting to even
        // without a scan hit: btm accepts the request (rc=0) and the user-side
        // channel then shows whether the stack does anything with it. Reboot
        // afterwards - btm keeps the request queued until it completes.
        BtdrvAddress configured;

        memset(&configured, 0, sizeof(configured));
        memcpy(configured.address, g_poc.target_address, sizeof(configured.address));

        pocLog("btm probe: no scan hit, connecting to the configured address anyway");

        if (pocBtmConnectRetry("btm probe (configured)", &configured, &handle)) {
            pocBtmLogGatt("btm probe (configured)", handle);

            if (g_btm_proto_ready) {
                pocBtmTransportStart(handle);
                pocBtmTransportPump(3000u);
                pocBtmTransportStop();
            } else {
                pocLog("btm probe: protocol coordinates missing, transport not started");
            }

            rc = btmBleDisconnect(handle);
            pocLog("btm probe (configured): BleDisconnect rc=0x%08X", (u32)rc);
        } else {
            pocBtmLogConnectionState("btm probe (configured)");
            pocBtmLogClientCondition("btm probe (configured)");

            // The 2026-09-24 runs show a second precondition beyond the client
            // activation gate: the same connect succeeded in the boot where the
            // driver-level probe (D-pad Left) had run first - it calls
            // btdrvInitializeBle + btdrvEnableBle, i.e. it turns the BLE stack on
            // - and failed with this same event when the btm probe ran alone.
            pocLog("btm probe: connect failed; if this is a fresh boot, run the "
                   "D-pad Left probe first (it enables the BLE stack), then retry");
        }
    } else {
        pocLog("btm probe: no scan hit and no configured address, connect skipped");
    }

    pocBtEventsDrain("btm probe after connect", 32u, NULL);
    pocBtEventsClose();

    if (have_scan_event)
        eventClose(&scan_event);

    btmExit();

    pocLog("btm probe: done");
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

        case DglabPocAction_ProbeBtmBle:
            // Runs before the session touches bt/btm:u/btdrv at all, so the btm
            // service sees a console state this run did not modify.
            pocLog("action: base btm BLE probe");
            w->probe_btm = true;
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
    // Printed by every session so a log says which sysmodule build produced it;
    // the probe versions below only appear when their key is pressed.
    pocLog("poc build: ble_poc v18 (base btm probe on StickR)");

    // The NRO sends START and the first ACTION back to back, so give that action
    // a moment to arrive before anything is opened or scanned. Collecting the
    // actions before btdevInitialize also lets the base-btm probe below run
    // before the session has opened bt, btm:u or btdrv at all.
    svcSleepThread(300000000ull); // 300ms

    bool any_action = false;
    while (pocTakeAction(w, &action)) {
        any_action = true;
        pocHandleAction(w, action);
    }

    // The base-btm probe owns the whole session: it must see the console the way
    // it is before this project touched Bluetooth (btm and the BLE manager keep
    // per-session state, docs/ble-poc.md), and there is nothing for the scan
    // loop to add to a btm answer. So it runs first, and the session ends here.
    if (w->probe_btm) {
        w->probe_btm = false;
        pocRunBtmBleProbe(w);
        goto out;
    }

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

    // The driver-level scan probe is no longer automatic: it calls
    // btdrvInitializeBle, and the BLE manager binds its internal connection to
    // the session that did that - once that session ends, every later BLE-side
    // command answers 0xF601 (KernelError_ConnectionClosed), which is what the
    // 2026-09-21 hardware logs in docs/ble-poc.md show. The Left key still runs
    // it on demand.

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

        if (w->probe_btm) {
            // The base-btm probe has to be the first Bluetooth access of its
            // session, and this one has already opened bt/btm:u and scanned.
            // Rather than read a state this run dirtied, ask for a fresh
            // session: the NRO starts one with the probes skipped when StickR is
            // pressed on the idle screen.
            w->probe_btm = false;
            pocLog("btm probe: needs a fresh session, press StickR on the idle screen");
            continue;
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
