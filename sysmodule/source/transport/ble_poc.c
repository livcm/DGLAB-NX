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

#define POC_EVENT_WAIT_MS 20u
#define POC_STEP_TIMEOUT_MS 8000u
#define POC_SCAN_TIMEOUT_MS 20000u
#define POC_CONNECT_TIMEOUT_MS 15000u
#define POC_B0_INTERVAL_MS 100u
#define POC_SCAN_MAX_ATTEMPTS 3u
#define POC_NOTIFY_LOG_LIMIT 6u
#define POC_SCAN_LOG_LIMIT 6u
#define POC_HEARTBEAT_MS 2000u
#define POC_POLL_INTERVAL_MS 100u
#define POC_POLL_LOG_EVERY 20u
#define POC_BTDEV_PROBE_MS 15000u

// Advertisement data types from the Bluetooth SIG "Supplement to the Bluetooth
// Core Specification". libnx documentation references a BtdrvAdType that is not
// actually defined in the installed headers, so the two values used here are
// written out instead of guessed from a libnx constant.
#define POC_AD_TYPE_UUID16_COMPLETE 0x03u // Complete list of 16-bit Service Class UUIDs
#define POC_AD_TYPE_NAME_SHORT 0x08u      // Shortened Local Name
#define POC_AD_TYPE_NAME_COMPLETE 0x09u   // Complete Local Name

// ---------------------------------------------------------------------------
// Worker state
// ---------------------------------------------------------------------------

enum {
    PocStep_Init = 0,
    PocStep_Scan,
    PocStep_RegisterClient,
    PocStep_Connect,
    PocStep_DiscoverServices,
    PocStep_DiscoverCharacteristics,
    PocStep_Subscribe,
    PocStep_BtdevProbe,
    PocStep_Connected,
    PocStep_Finished,
};

// Everything in here is owned by the worker thread and is reset at the start of
// every run, so the thread handle deliberately does not live here. Shared state
// lives in PocShared and is only touched under its mutex.
typedef struct {
    Event ble_event;
    bool ble_event_active;
    bool btdrv_ready;
    bool hid_event_path;    // true once the LE HID event queue is used instead
    bool tried_hid_path;
    bool wait_error_logged;

    u32 step;
    u32 step_start_ms;
    u32 scan_attempts;
    u32 next_b0_ms;
    u32 last_heartbeat_ms;
    u32 last_poll_ms;
    u32 poll_count;
    Result last_poll_rc[2];

    bool scanning;
    bool connected;
    bool self_disconnect; // set when the PoC itself asks for the disconnect
    bool client_registered;
    bool have_service;
    bool have_battery_service;
    u8 client_if;
    u32 conn_id;
    BtdrvAddress address;
    u8 ble_addr_type;
    BtdrvGattId service_id;
    BtdrvGattId battery_service_id;
    BtdrvGattId char_write_id;
    BtdrvGattId char_notify_id;
    BtdrvGattId char_battery_id;
    u32 notify_logged;
    u32 scan_logged;
    BtdrvAddress last_scan_address;
    u8 last_scan_addr_type;
    bool have_last_scan;
} PocWorker;

typedef struct {
    Mutex mutex;

    // Guarded by mutex.
    DglabPocStatus status;
    u32 log_write_offset;
    u32 log_valid_from; // first offset that still holds current-run text
    char log[POC_LOG_CAPACITY];
    Thread worker_thread;
    bool running;
    bool stop_requested;
    bool auto_write;
    u64 aruid;
    u32 pending_action;

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

static bool pocElapsed(u32 start_ms, u32 timeout_ms)
{
    return (u32)(pocNowMs() - start_ms) >= timeout_ms;
}

// Builds the 128-bit form of a 16-bit Bluetooth UUID using the base UUID from
// the official DG-LAB documentation. The full 16-byte form is used everywhere
// because libnx does not document whether a 2-byte UUID in
// BtdrvGattAttributeUuid is stored little or big endian, while the 16-byte form
// is unambiguous.
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

// Matches an attribute UUID against a 16-bit UUID. Both byte orders are
// accepted for the 2-byte form, because the stack may report either encoding and
// the PoC log records what was actually seen.
static bool pocUuidIs16(const BtdrvGattAttributeUuid* uuid, u16 value)
{
    if (uuid->size == 0x2) {
        u8 hi = (u8)(value >> 8);
        u8 lo = (u8)(value & 0xFF);

        return (uuid->uuid[0] == hi && uuid->uuid[1] == lo) ||
               (uuid->uuid[0] == lo && uuid->uuid[1] == hi);
    }

    if (uuid->size == 0x10) {
        BtdrvGattAttributeUuid want = pocUuid16(value);

        return memcmp(uuid->uuid, want.uuid, 0x10) == 0;
    }

    return false;
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

// ---------------------------------------------------------------------------
// Status counters
// ---------------------------------------------------------------------------

static void pocRecordScanResult(void)
{
    mutexLock(&g_poc.mutex);
    g_poc.status.scan_results++;
    mutexUnlock(&g_poc.mutex);
}

// Counts every event the stack hands us, whatever its type. Without this a scan
// that produces nothing looks the same as an event path that never fires.
static void pocRecordEvent(BtdrvBleEventType type)
{
    mutexLock(&g_poc.mutex);
    g_poc.status.event_count++;
    g_poc.status.last_event_type = (u32)type;
    mutexUnlock(&g_poc.mutex);
}

static void pocRecordMatch(const BtdrvAddress* addr, u8 ble_addr_type)
{
    mutexLock(&g_poc.mutex);
    g_poc.status.scan_matched++;
    memcpy(g_poc.status.address, addr->address, sizeof(g_poc.status.address));
    g_poc.status.address_valid = 1;
    g_poc.status.ble_addr_type = ble_addr_type;
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
// Worker: helpers
// ---------------------------------------------------------------------------

static bool pocAdvertisementMatches(const BtdrvBleAdvertisement* ad)
{
    const char* name = DGLAB_COYOTE_V3_DEVICE_NAME;
    const size_t name_len = 9; // "47L121000"

    if (ad->size > sizeof(ad->data))
        return false;

    if (ad->type == POC_AD_TYPE_NAME_COMPLETE && ad->size >= name_len &&
        memcmp(ad->data, name, name_len) == 0)
        return true;

    if (ad->type == POC_AD_TYPE_NAME_SHORT && ad->size >= 4 && memcmp(ad->data, name, 4) == 0)
        return true;

    if (ad->type == POC_AD_TYPE_UUID16_COMPLETE) {
        for (u32 i = 0; i + 1 < ad->size; i += 2) {
            u16 value = (u16)(ad->data[i] | (ad->data[i + 1] << 8));
            u16 swapped = (u16)((value >> 8) | (value << 8));

            if (value == DGLAB_COYOTE_V3_UUID16_SERVICE ||
                swapped == DGLAB_COYOTE_V3_UUID16_SERVICE)
                return true;
        }
    }

    return false;
}

// A B0 packet that changes nothing: no strength change, both channels idle. The
// device discards invalid channel data, so this exercises the write path without
// producing output.
static void pocWriteIdleB0(void)
{
    DglabCoyoteV3B0 b0;
    u8 packet[DGLAB_COYOTE_V3_B0_SIZE];
    Result rc;
    u32 written;

    memset(&b0, 0, sizeof(b0));
    dglabCoyoteV3EncodeB0(&b0, packet);

    rc = btdrvWriteGattCharacteristic(g_poc.worker.conn_id, true, &g_poc.worker.service_id,
        &g_poc.worker.char_write_id, packet, sizeof(packet), BtdrvGattAuthReqType_None, false);

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
static void pocWriteZeroB0(void)
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
    rc = btdrvWriteGattCharacteristic(g_poc.worker.conn_id, true, &g_poc.worker.service_id,
        &g_poc.worker.char_write_id, packet, sizeof(packet), BtdrvGattAuthReqType_None, true);
    pocRecordB0Write(rc);
    pocLog("b0 zero write rc=0x%08X", (u32)rc);
}

static void pocReadBattery(void)
{
    PocWorker* w = &g_poc.worker;
    Result rc;

    if (!w->have_battery_service || w->char_battery_id.uuid.size == 0) {
        pocLog("battery read skipped: characteristic not resolved");
        return;
    }

    rc = btdrvReadGattCharacteristic(w->conn_id, true, &w->battery_service_id,
        &w->char_battery_id, BtdrvGattAuthReqType_None);
    pocLog("battery read rc=0x%08X", (u32)rc);
}

// ---------------------------------------------------------------------------
// Worker: step machine
// ---------------------------------------------------------------------------

static bool pocEnterStep(PocWorker* w, u32 step);
static void pocHandleEvent(PocWorker* w, BtdrvBleEventType type, const BtdrvBleEventInfo* info);

// Acquires the BLE event handle. btdrv exposes two independent event queues
// that carry the same BtdrvBleEventInfo payloads, so the second one is used as
// a fallback when the first one never delivers anything.
static Result pocAcquireBleEvent(PocWorker* w, bool hid_path)
{
    Result rc;

    if (w->ble_event_active) {
        eventClose(&w->ble_event);
        w->ble_event_active = false;
    }

    w->hid_event_path = hid_path;
    w->wait_error_logged = false;

    if (hid_path)
        rc = btdrvRegisterBleHidEvent(&w->ble_event);
    else
        rc = btdrvInitializeBle(&w->ble_event);

    if (R_SUCCEEDED(rc))
        w->ble_event_active = true;

    return rc;
}

// Waits for one BLE event. Returns false on timeout, which is the normal case
// while a step is making no progress.
static bool pocWaitEvent(BtdrvBleEventType* out_type, BtdrvBleEventInfo* out_info, u32 timeout_ms)
{
    PocWorker* w = &g_poc.worker;
    Result rc;

    if (!w->ble_event_active)
        return false;

    rc = eventWait(&w->ble_event, (u64)timeout_ms * 1000000ull);
    if (R_FAILED(rc)) {
        // Log the first failure so a broken handle is distinguishable from the
        // ordinary timeout that a quiet event queue produces.
        if (!w->wait_error_logged) {
            w->wait_error_logged = true;
            pocLog("eventWait first failure rc=0x%08X (timeout is normal)", (u32)rc);
        }
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));

    if (w->hid_event_path)
        rc = btdrvGetLeHidEventInfo(out_info, sizeof(*out_info), out_type);
    else
        rc = btdrvGetBleManagedEventInfo(out_info, sizeof(*out_info), out_type);

    if (R_FAILED(rc)) {
        pocLog("ble event fetch rc=0x%08X", (u32)rc);
        return false;
    }

    return true;
}

// Asks both event queues for data without waiting on the event handle, purely
// for the log.
//
// This must never feed the state machine: on HOS 22.5.0 an empty queue answers
// with success and type 0, which the state machine would read as a real
// registration event. An earlier revision did exactly that and drove a bogus
// connect to 00:00:00:00:00:00, so the results here are log-only.
static void pocPollEventQueues(PocWorker* w)
{
    static const bool kHidQueue[2] = { false, true };

    for (u32 i = 0; i < 2; i++) {
        BtdrvBleEventInfo info;
        BtdrvBleEventType type = 0;
        Result rc;

        memset(&info, 0, sizeof(info));

        if (kHidQueue[i])
            rc = btdrvGetLeHidEventInfo(&info, sizeof(info), &type);
        else
            rc = btdrvGetBleManagedEventInfo(&info, sizeof(info), &type);

        bool log_now = R_SUCCEEDED(rc) || (w->poll_count % POC_POLL_LOG_EVERY) == 0 ||
                       rc != w->last_poll_rc[i];

        w->last_poll_rc[i] = rc;

        if (log_now) {
            pocLog("poll %s rc=0x%08X type=%u", kHidQueue[i] ? "lehid" : "managed", (u32)rc,
                (u32)type);
        }

    }

    w->poll_count++;
}

static void pocHandleCacheSave(PocWorker* w, const BtdrvBleEventInfo* info)
{
    u32 count = info->client_cache_save.count;

    if (count > 10)
        count = 10;

    pocLog("event cache save result=0x%08X conn=%u count=%u", info->client_cache_save.result,
        info->client_cache_save.conn_id, count);

    for (u32 i = 0; i < count; i++) {
        const BtdrvGattAttribute* attr = &info->client_cache_save.attr_list[i];
        u16 value = (u16)(attr->id.uuid.uuid[0] | (attr->id.uuid.uuid[1] << 8));

        if (attr->id.uuid.size != 0x2)
            value = 0;

        pocLog("  attr %u type=%u uuid_size=0x%X uuid16=0x%04X handle=0x%04X prop=0x%02X", i,
            attr->type, attr->id.uuid.size, value, attr->handle, attr->property);

        if (attr->type != BtdrvGattAttributeType_Service)
            continue;

        if (pocUuidIs16(&attr->id.uuid, DGLAB_COYOTE_V3_UUID16_SERVICE)) {
            w->service_id = attr->id;
            w->have_service = true;
        } else if (pocUuidIs16(&attr->id.uuid, DGLAB_COYOTE_V3_UUID16_BATTERY_SERVICE)) {
            w->battery_service_id = attr->id;
            w->have_battery_service = true;
        }
    }

    if (w->have_service) {
        pocSetMilestone(DGLAB_POC_MILESTONE_SERVICE_FOUND);
        pocLog("service 0x180C resolved");
        pocEnterStep(w, PocStep_DiscoverCharacteristics);
    }
}

static void pocHandleNotify(PocWorker* w, const BtdrvBleEventInfo* info)
{
    const BtdrvBleEventInfo* ev = info;
    u32 size = ev->client_notify.size;
    char hex[3 * 20 + 1];

    if (size > sizeof(ev->client_notify.data))
        size = sizeof(ev->client_notify.data);

    if (size > 20)
        size = 20;

    pocHex(hex, sizeof(hex), ev->client_notify.data, size);

    mutexLock(&g_poc.mutex);
    g_poc.status.notify_count++;
    g_poc.status.last_notify_size = size;
    memcpy(g_poc.status.last_notify, ev->client_notify.data, size);
    mutexUnlock(&g_poc.mutex);

    if (w->notify_logged < POC_NOTIFY_LOG_LIMIT) {
        pocLog("notify type=%u size=%u data=%s", ev->client_notify.type, size, hex);
        w->notify_logged++;
    }

    if (size >= DGLAB_COYOTE_V3_B1_SIZE && ev->client_notify.data[0] == DGLAB_COYOTE_V3_HEADER_B1) {
        DglabCoyoteV3B1 b1;

        pocSetMilestone(DGLAB_POC_MILESTONE_B1_RECEIVED);

        if (dglabCoyoteV3DecodeB1(ev->client_notify.data, size, &b1)) {
            pocLog("B1 sequence=%u A=%u B=%u", b1.sequence, b1.strength_a, b1.strength_b);
        }
    } else if (size >= 1 && pocUuidIs16(&ev->client_notify.char_uuid, DGLAB_COYOTE_V3_UUID16_CHAR_BATTERY)) {
        mutexLock(&g_poc.mutex);
        g_poc.status.battery_value = ev->client_notify.data[0];
        g_poc.status.battery_valid = 1;
        g_poc.status.milestone |= DGLAB_POC_MILESTONE_BATTERY_READ;
        mutexUnlock(&g_poc.mutex);
        pocLog("battery value=%u", ev->client_notify.data[0]);
    }
}

static void pocHandleScanResult(PocWorker* w, const BtdrvBleEventInfo* info)
{
    const BtdrvBleEventInfo* ev = info;
    u32 count = ev->scan_result.count;

    if (count > 10)
        count = 10;

    pocRecordScanResult();

    // Remember the newest address even when the advertisement does not match,
    // so the fallback action can still drive the GATT part of the PoC.
    w->last_scan_address = ev->scan_result.address;
    w->last_scan_addr_type = ev->scan_result.ble_addr_type;
    w->have_last_scan = true;

    if (w->scan_logged < POC_SCAN_LOG_LIMIT) {
        pocLog("scan status=%u addr=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d entries=%u",
            ev->scan_result.status, ev->scan_result.address.address[0],
            ev->scan_result.address.address[1], ev->scan_result.address.address[2],
            ev->scan_result.address.address[3], ev->scan_result.address.address[4],
            ev->scan_result.address.address[5], ev->scan_result.rssi, count);
        w->scan_logged++;
    }

    for (u32 i = 0; i < count; i++) {
        char hex[3 * 0x1D + 1];
        u32 size = ev->scan_result.ad_list[i].size;

        if (size > sizeof(ev->scan_result.ad_list[i].data))
            size = sizeof(ev->scan_result.ad_list[i].data);

        pocHex(hex, sizeof(hex), ev->scan_result.ad_list[i].data, size);

        if (w->scan_logged < POC_SCAN_LOG_LIMIT) {
            pocLog("  ad type=0x%02X size=%u data=%s", ev->scan_result.ad_list[i].type,
                ev->scan_result.ad_list[i].size, hex);
            w->scan_logged++;
        }

        if (!pocAdvertisementMatches(&ev->scan_result.ad_list[i]))
            continue;

        w->address = ev->scan_result.address;
        w->ble_addr_type = ev->scan_result.ble_addr_type;
        pocRecordMatch(&ev->scan_result.address, ev->scan_result.ble_addr_type);
        pocSetMilestone(DGLAB_POC_MILESTONE_DEVICE_FOUND);
        pocLog("coyote 3.0 found");
        pocEnterStep(w, PocStep_RegisterClient);
        return;
    }
}

static void pocHandleEvent(PocWorker* w, BtdrvBleEventType type, const BtdrvBleEventInfo* info)
{
    pocRecordEvent(type);

    switch (type) {
        case BtdrvBleEventType_ClientRegistration:
            pocLog("event client registration result=0x%08X status=%u client_if=%u",
                info->client_registration.result, info->client_registration.status,
                info->client_registration.client_if);

            if (info->client_registration.status == 1) {
                w->client_if = info->client_registration.client_if;
                w->client_registered = true;

                mutexLock(&g_poc.mutex);
                g_poc.status.client_if = w->client_if;
                mutexUnlock(&g_poc.mutex);

                pocSetMilestone(DGLAB_POC_MILESTONE_CLIENT_READY);
                pocEnterStep(w, PocStep_Connect);
            }
            break;

        case BtdrvBleEventType_ClientConnection:
            pocLog("event client connection result=0x%08X status=%u conn_id=%u reason=%u",
                info->client_connection.result, info->client_connection.status,
                info->client_connection.conn_id, info->client_connection.reason);

            if (info->client_connection.status == 0) {
                w->conn_id = info->client_connection.conn_id;
                w->connected = true;

                mutexLock(&g_poc.mutex);
                g_poc.status.conn_id = w->conn_id;
                mutexUnlock(&g_poc.mutex);

                pocSetMilestone(DGLAB_POC_MILESTONE_CONNECTED);
                pocEnterStep(w, PocStep_DiscoverServices);
            } else {
                w->connected = false;

                // A disconnect the PoC asked for (reconnect with another
                // ARUID) is expected; anything else ends the run so the reason
                // stays visible in the log.
                if (w->self_disconnect) {
                    w->self_disconnect = false;
                } else {
                    pocLog("disconnected by the device, ending run");
                    w->step = PocStep_Finished;
                }
            }
            break;

        case BtdrvBleEventType_ClientCacheSave:
            pocHandleCacheSave(w, info);
            break;

        case BtdrvBleEventType_ClientNotify:
            pocHandleNotify(w, info);
            break;

        case BtdrvBleEventType_ClientConfigureMtu:
            pocLog("event configure mtu result=0x%08X conn=%u mtu=%u", info->client_configure_mtu.result,
                info->client_configure_mtu.conn_id, info->client_configure_mtu.mtu);

            mutexLock(&g_poc.mutex);
            g_poc.status.mtu = info->client_configure_mtu.mtu;
            mutexUnlock(&g_poc.mutex);
            break;

        case BtdrvBleEventType_ScanResult:
            pocHandleScanResult(w, info);
            break;

        default:
            pocLog("event type=%u result=0x%08X", (u32)type, info->client_registration.result);
            break;
    }
}

static bool pocEnterStep(PocWorker* w, u32 step)
{
    Result rc;

    w->step = step;
    w->step_start_ms = pocNowMs();

    switch (step) {
        case PocStep_Scan: {
            rc = btdrvStartBleScan();
            pocLog("btdrvStartBleScan rc=0x%08X", (u32)rc);

            if (R_FAILED(rc)) {
                pocFail(rc, "startBleScan");
                return false;
            }

            w->scanning = true;
            w->last_heartbeat_ms = pocNowMs();
            pocSetMilestone(DGLAB_POC_MILESTONE_SCAN_STARTED);
            pocSetState(DglabPocState_Scanning);
            break;
        }

        case PocStep_RegisterClient: {
            BtdrvGattAttributeUuid uuid = pocUuid16(DGLAB_COYOTE_V3_UUID16_SERVICE);

            if (w->scanning) {
                rc = btdrvStopBleScan();
                pocLog("btdrvStopBleScan rc=0x%08X", (u32)rc);
                w->scanning = false;
            }

            if (w->client_registered) {
                pocLog("gatt client already registered, reconnecting");
                pocEnterStep(w, PocStep_Connect);
                break;
            }

            rc = btdrvRegisterGattClient(&uuid);
            pocLog("btdrvRegisterGattClient rc=0x%08X", (u32)rc);

            if (R_FAILED(rc)) {
                pocFail(rc, "registerGattClient");
                return false;
            }

            pocSetState(DglabPocState_Registering);
            break;
        }

        case PocStep_Connect: {
            rc = btdrvConnectGattServer(w->client_if, w->address, true, g_poc.aruid);
            pocLog("btdrvConnectGattServer rc=0x%08X aruid_low=0x%08X", (u32)rc, (u32)g_poc.aruid);

            if (R_FAILED(rc)) {
                pocFail(rc, "connectGattServer");
                return false;
            }

            pocSetState(DglabPocState_Connecting);
            break;
        }

        case PocStep_DiscoverServices: {
            BtdrvGattAttributeUuid service = pocUuid16(DGLAB_COYOTE_V3_UUID16_SERVICE);
            BtdrvGattAttributeUuid battery = pocUuid16(DGLAB_COYOTE_V3_UUID16_BATTERY_SERVICE);

            rc = btdrvGetGattService(w->conn_id, &service);
            pocLog("btdrvGetGattService(0x180C) rc=0x%08X", (u32)rc);

            Result battery_rc = btdrvGetGattService(w->conn_id, &battery);
            pocLog("btdrvGetGattService(0x180A) rc=0x%08X", (u32)battery_rc);

            if (R_FAILED(rc)) {
                pocFail(rc, "getGattService");
                return false;
            }

            pocSetState(DglabPocState_Discovering);
            break;
        }

        case PocStep_DiscoverCharacteristics: {
            BtdrvGattAttributeUuid filter_write = pocUuid16(DGLAB_COYOTE_V3_UUID16_CHAR_WRITE);
            BtdrvGattAttributeUuid filter_notify = pocUuid16(DGLAB_COYOTE_V3_UUID16_CHAR_NOTIFY);
            BtdrvGattAttributeUuid filter_battery = pocUuid16(DGLAB_COYOTE_V3_UUID16_CHAR_BATTERY);
            u8 property = 0;

            rc = btdrvGetGattFirstCharacteristic(w->conn_id, &w->service_id, true, &filter_write,
                &property, &w->char_write_id);
            pocLog("char 0x150A rc=0x%08X size=0x%X prop=0x%02X", (u32)rc, w->char_write_id.uuid.size,
                property);

            mutexLock(&g_poc.mutex);
            g_poc.status.char_write_prop = property;
            mutexUnlock(&g_poc.mutex);

            property = 0;

            Result notify_rc = btdrvGetGattFirstCharacteristic(w->conn_id, &w->service_id, true,
                &filter_notify, &property, &w->char_notify_id);
            pocLog("char 0x150B rc=0x%08X size=0x%X prop=0x%02X", (u32)notify_rc,
                w->char_notify_id.uuid.size, property);

            mutexLock(&g_poc.mutex);
            g_poc.status.char_notify_prop = property;
            mutexUnlock(&g_poc.mutex);

            if (w->have_battery_service) {
                property = 0;

                Result battery_rc = btdrvGetGattFirstCharacteristic(w->conn_id,
                    &w->battery_service_id, true, &filter_battery, &property, &w->char_battery_id);
                pocLog("char 0x1500 rc=0x%08X size=0x%X prop=0x%02X", (u32)battery_rc,
                    w->char_battery_id.uuid.size, property);

                mutexLock(&g_poc.mutex);
                g_poc.status.char_battery_prop = property;
                mutexUnlock(&g_poc.mutex);
            }

            if (R_FAILED(rc) || w->char_write_id.uuid.size == 0) {
                pocFail(rc, "characteristic 0x150A");
                return false;
            }

            if (R_FAILED(notify_rc) || w->char_notify_id.uuid.size == 0) {
                pocFail(notify_rc, "characteristic 0x150B");
                return false;
            }

            pocSetMilestone(DGLAB_POC_MILESTONE_CHARS_FOUND);
            pocEnterStep(w, PocStep_Subscribe);
            break;
        }

        case PocStep_Subscribe: {
            rc = btdrvRegisterGattNotification(w->conn_id, true, &w->service_id, &w->char_notify_id);
            pocLog("btdrvRegisterGattNotification rc=0x%08X", (u32)rc);

            if (R_FAILED(rc)) {
                pocFail(rc, "registerNotification");
                return false;
            }

            pocSetMilestone(DGLAB_POC_MILESTONE_NOTIFY_ON);
            pocSetState(DglabPocState_Ready);
            pocLog("session ready, auto_write=%u", g_poc.auto_write ? 1u : 0u);

            w->next_b0_ms = pocNowMs();
            w->step = PocStep_Connected;
            break;
        }

        default:
            break;
    }

    return true;
}

// Diagnostic: run the same scan through libnx's btdev wrapper (bt + btm:u)
// instead of btdrv.
//
// The btdrv event queue produced only empty payloads on HOS 22.5.0, so this
// decides whether the higher level service can be used from the sysmodule. The
// result also tells us whether the ARUID that bt/btm:u pass internally (0 in a
// background process) is accepted.
static void pocRunBtdevProbe(PocWorker* w)
{
    BtdrvGattAttributeUuid service = pocUuid16(DGLAB_COYOTE_V3_UUID16_SERVICE);
    Event scan_event;
    u32 results_seen = 0;
    u32 deadline;
    Result rc;

    (void)w;

    pocLog("btdev probe: btInitialize + btmuInitialize");
    rc = btdevInitialize();
    pocLog("btdevInitialize rc=0x%08X", (u32)rc);

    if (R_FAILED(rc)) {
        pocLog("btdev probe: bt/btm:u unusable from this process");
        return;
    }

    memset(&scan_event, 0, sizeof(scan_event));

    Result event_rc = btdevAcquireBleScanEvent(&scan_event);
    pocLog("btdevAcquireBleScanEvent rc=0x%08X", (u32)event_rc);

    rc = btdevStartBleScanSmartDevice(&service);
    pocLog("btdevStartBleScanSmartDevice(0x180C) rc=0x%08X", (u32)rc);

    deadline = pocNowMs() + POC_BTDEV_PROBE_MS;

    while (pocNowMs() < deadline) {
        BtdrvBleScanResult results[10];
        u8 total = 0;

        mutexLock(&g_poc.mutex);
        bool stop = g_poc.stop_requested;
        mutexUnlock(&g_poc.mutex);

        if (stop)
            break;

        if (R_SUCCEEDED(event_rc))
            eventWait(&scan_event, 500ull * 1000000ull);
        else
            svcSleepThread(500000000ull);

        memset(results, 0, sizeof(results));

        Result get_rc = btdevGetBleScanResult(results, 10, &total);
        pocLog("btdevGetBleScanResult rc=0x%08X count=%u", (u32)get_rc, total);

        for (u8 i = 0; i < total && i < 10; i++) {
            pocLog("  scan %u addr=%02X:%02X:%02X:%02X:%02X:%02X count=%d", i,
                results[i].addr.address[0], results[i].addr.address[1],
                results[i].addr.address[2], results[i].addr.address[3],
                results[i].addr.address[4], results[i].addr.address[5], results[i].count);
            results_seen++;
        }
    }

    rc = btdevStopBleScanSmartDevice();
    pocLog("btdevStopBleScanSmartDevice rc=0x%08X", (u32)rc);

    if (R_SUCCEEDED(event_rc))
        eventClose(&scan_event);

    btdevExit();

    pocLog("btdev probe: done, %u results", results_seen);
}

static void pocHandlePendingAction(PocWorker* w)
{
    u32 action;

    mutexLock(&g_poc.mutex);
    action = g_poc.pending_action;
    g_poc.pending_action = 0;
    mutexUnlock(&g_poc.mutex);

    switch (action) {
        case DglabPocAction_WriteIdleB0:
            if (!w->connected) {
                pocLog("action ignored, not connected");
                break;
            }

            pocLog("action write idle b0");
            pocWriteIdleB0();
            break;

        case DglabPocAction_WriteZeroB0:
            if (!w->connected) {
                pocLog("action ignored, not connected");
                break;
            }

            pocLog("action write zero b0");
            pocWriteZeroB0();
            break;

        case DglabPocAction_ReadBattery:
            if (!w->connected) {
                pocLog("action ignored, not connected");
                break;
            }

            pocLog("action read battery");
            pocReadBattery();
            break;

        case DglabPocAction_ToggleAutoWrite:
            mutexLock(&g_poc.mutex);
            g_poc.auto_write = !g_poc.auto_write;
            u32 enabled = g_poc.auto_write ? 1u : 0u;
            g_poc.status.auto_write = enabled;
            mutexUnlock(&g_poc.mutex);
            pocLog("action auto_write=%u", enabled);
            break;

        case DglabPocAction_ReconnectAruid0:
            pocLog("action reconnect with aruid 0");

            if (w->connected) {
                w->self_disconnect = true;
                btdrvDisconnectGattServer(w->conn_id);
                w->connected = false;
            }

            g_poc.aruid = 0;

            mutexLock(&g_poc.mutex);
            g_poc.status.aruid_low = 0;
            mutexUnlock(&g_poc.mutex);

            pocEnterStep(w, PocStep_Connect);
            break;

        // Some of the scan state lives in the shared Bluetooth stack, so when a
        // scan produces nothing the first experiment is to clear and disable the
        // filters the stack may still be applying from another user.
        case DglabPocAction_RescanNoFilter: {
            Result clear_rc = btdrvClearBleScanFilters();
            Result filter_rc = btdrvEnableBleScanFilter(false);

            pocLog("scan filter off: clear rc=0x%08X enable rc=0x%08X", (u32)clear_rc,
                (u32)filter_rc);

            if (w->scanning) {
                btdrvStopBleScan();
                w->scanning = false;
            }

            w->scan_attempts = 0;
            pocEnterStep(w, PocStep_Scan);
            break;
        }

        // Fallback that keeps the GATT half of the PoC usable even when the
        // advertisement filter never matches.
        case DglabPocAction_ConnectLastScan:
            if (!w->have_last_scan) {
                pocLog("connect last scan: no scan result seen yet");
                break;
            }

            w->address = w->last_scan_address;
            w->ble_addr_type = w->last_scan_addr_type;
            pocRecordMatch(&w->address, w->ble_addr_type);
            pocSetMilestone(DGLAB_POC_MILESTONE_DEVICE_FOUND);

            pocLog("connecting to last scanned device %02X:%02X:%02X:%02X:%02X:%02X",
                w->address.address[0], w->address.address[1], w->address.address[2],
                w->address.address[3], w->address.address[4], w->address.address[5]);

            if (w->scanning) {
                btdrvStopBleScan();
                w->scanning = false;
            }

            if (w->client_registered)
                pocEnterStep(w, PocStep_Connect);
            else
                pocEnterStep(w, PocStep_RegisterClient);
            break;

        case DglabPocAction_Disconnect:
            pocLog("action disconnect");
            w->step = PocStep_Finished;
            break;

        case DglabPocAction_ProbeBtdev:
            pocLog("action probe btdev");

            if (w->scanning) {
                btdrvStopBleScan();
                w->scanning = false;
            }

            w->step = PocStep_BtdevProbe;
            break;

        default:
            break;
    }
}

// Returns false when the run has ended.
static bool pocStepRun(PocWorker* w)
{
    switch (w->step) {
        case PocStep_Scan:
            // Heartbeat so a silent scan is distinguishable from a broken event
            // path when reading the log later.
            if (pocElapsed(w->last_heartbeat_ms, POC_HEARTBEAT_MS)) {
                u32 events;
                u32 results;
                u32 last_type;

                w->last_heartbeat_ms = pocNowMs();

                mutexLock(&g_poc.mutex);
                events = g_poc.status.event_count;
                results = g_poc.status.scan_results;
                last_type = g_poc.status.last_event_type;
                mutexUnlock(&g_poc.mutex);

                pocLog("scanning events=%u results=%u last_event=%u", events, results, last_type);

                if (events == 0)
                    pocLog("no BLE events received at all yet");

                // Prove whether the queue is empty or merely not signalled.
                pocPollEventQueues(w);
            }

            if (w->scanning && pocElapsed(w->step_start_ms, POC_SCAN_TIMEOUT_MS)) {
                w->scan_attempts++;

                // Nothing at all arrived on the managed event queue: try the
                // other event queue btdrv exposes before giving up.
                if (w->scan_attempts == 1 && !w->tried_hid_path) {
                    pocLog("no events after %u ms, switching to the LE HID event source",
                        POC_SCAN_TIMEOUT_MS);

                    Result event_rc = pocAcquireBleEvent(w, true);
                    pocLog("btdrvRegisterBleHidEvent rc=0x%08X", (u32)event_rc);
                    w->tried_hid_path = true;

                    if (w->scanning) {
                        btdrvStopBleScan();
                        w->scanning = false;
                    }

                    return pocEnterStep(w, PocStep_Scan);
                }

                if (w->scan_attempts >= POC_SCAN_MAX_ATTEMPTS) {
                    pocFail(MAKERESULT(Module_Libnx, LibnxError_Timeout), "scan timeout");
                    return false;
                }

                pocLog("scan restart %u", w->scan_attempts);
                btdrvStopBleScan();
                w->scanning = false;
                return pocEnterStep(w, PocStep_Scan);
            }
            break;

        case PocStep_RegisterClient:
            if (pocElapsed(w->step_start_ms, POC_STEP_TIMEOUT_MS)) {
                pocFail(MAKERESULT(Module_Libnx, LibnxError_Timeout), "client registration timeout");
                return false;
            }
            break;

        case PocStep_Connect:
            if (pocElapsed(w->step_start_ms, POC_CONNECT_TIMEOUT_MS)) {
                pocFail(MAKERESULT(Module_Libnx, LibnxError_Timeout), "connect timeout");
                return false;
            }
            break;

        case PocStep_DiscoverServices:
            if (pocElapsed(w->step_start_ms, POC_STEP_TIMEOUT_MS)) {
                pocFail(MAKERESULT(Module_Libnx, LibnxError_Timeout), "service discovery timeout");
                return false;
            }
            break;

        case PocStep_Connected: {
            u32 now = pocNowMs();

            mutexLock(&g_poc.mutex);
            bool auto_write = g_poc.auto_write;
            mutexUnlock(&g_poc.mutex);

            if (auto_write && w->connected && (s32)(now - w->next_b0_ms) >= 0) {
                w->next_b0_ms = now + POC_B0_INTERVAL_MS;
                pocWriteIdleB0();
            }
            break;
        }

        case PocStep_Finished:
            return false;

        case PocStep_BtdevProbe:
            // Diagnostic run: report and end the session.
            pocRunBtdevProbe(w);
            return false;

        default:
            break;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void pocCleanup(PocWorker* w)
{
    if (w->scanning) {
        btdrvStopBleScan();
        w->scanning = false;
    }

    if (w->connected && w->conn_id != 0) {
        btdrvDisconnectGattServer(w->conn_id);
        w->connected = false;
    }

    if (w->client_if != 0)
        btdrvUnregisterGattClient(w->client_if);

    if (w->ble_event_active) {
        eventClose(&w->ble_event);
        w->ble_event_active = false;
    }

    if (w->btdrv_ready) {
        btdrvExit();
        w->btdrv_ready = false;
    }
}

static void pocThreadFunc(void* arg)
{
    PocWorker* w = &g_poc.worker;
    Result rc;

    (void)arg;

    memset(w, 0, sizeof(*w));
    w->step = PocStep_Init;
    w->step_start_ms = pocNowMs();

    pocSetState(DglabPocState_Initializing);
    pocLog("poc start aruid_low=0x%08X", (u32)g_poc.aruid);

    rc = btdrvInitialize();
    if (R_FAILED(rc)) {
        pocFail(rc, "btdrvInitialize");
        goto out;
    }
    w->btdrv_ready = true;

    rc = pocAcquireBleEvent(w, false);
    if (R_FAILED(rc)) {
        pocFail(rc, "btdrvInitializeBle");
        goto out;
    }
    pocSetMilestone(DGLAB_POC_MILESTONE_BLE_READY);
    pocLog("ble event source: managed (btdrvInitializeBle)");

    bool enabled = false;
    rc = btdrvIsBluetoothEnabled(&enabled);
    pocLog("bluetooth adapter enabled rc=0x%08X value=%u", (u32)rc, enabled ? 1u : 0u);

    // Always ask for BLE explicitly. btdrvIsBluetoothEnabled reports the
    // adapter, not whether the LE host is running, and a scan started while LE
    // is idle returns success but never produces an event. The first hardware
    // run showed exactly that: zero events, forever.
    Result enable_rc = btdrvEnableBle();
    pocLog("btdrvEnableBle rc=0x%08X", (u32)enable_rc);

    // The event handle is acquired before the scan starts, so no BLE activity
    // happens until the caller explicitly asks for it.
    pocEnterStep(w, PocStep_Scan);

    while (true) {
        BtdrvBleEventType type = 0;
        BtdrvBleEventInfo info;

        mutexLock(&g_poc.mutex);
        bool stop = g_poc.stop_requested;
        mutexUnlock(&g_poc.mutex);

        if (stop) {
            pocLog("stop requested");
            break;
        }

        if (pocWaitEvent(&type, &info, POC_EVENT_WAIT_MS))
            pocHandleEvent(w, type, &info);

        // Actions are honoured in any step so that "disconnect" and "stop
        // writing" also work while a later step is still making progress.
        pocHandlePendingAction(w);

        if (!pocStepRun(w))
            break;
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

    // If a previous run has finished, its thread handle is still open.
    Thread previous = g_poc.worker_thread;

    mutexUnlock(&g_poc.mutex);

    if (previous.handle != INVALID_HANDLE) {
        threadWaitForExit(&previous);
        threadClose(&previous);
    }

    mutexLock(&g_poc.mutex);
    // Drop the closed handle so a later start can never close it twice.
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

    if (!running)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

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
