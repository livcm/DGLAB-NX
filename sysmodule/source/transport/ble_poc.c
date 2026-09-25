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
#include <sys/stat.h>

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
    // Which probe this session should run. Both are one-shot: the thread runs
    // the probe and the session ends, because the two probes must not share a
    // session (the driver-level one owns the BLE stack bring-up).
    bool probe_btdrv;
    bool probe_btm;
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
    u64 aruid;
    u32 pending_action;
    bool use_target_address;
    bool address_discovered; // The driver-level scan already wrote one out.
    u8 target_address[6];

    // Worker owned.
    PocWorker worker;
} PocShared;

static PocShared g_poc;

// The worker thread is created on demand, so its stack cannot live on the stack
// of whichever thread starts it.
static u8 g_poc_thread_stack[POC_THREAD_STACK_SIZE] __attribute__((aligned(0x1000)));

// The managed BLE event payload is 0x400 bytes. It lives in .bss instead of on
// the worker's stack because tests/stack exists to fail new KB-scale frames
// (sysmodule/AGENTS.md, "线程与栈"); only the PoC worker thread touches it.
static BtdrvBleEventInfo g_ble_event;

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

// ---------------------------------------------------------------------------
// Event helper
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Outgoing B0 traffic
// ---------------------------------------------------------------------------

// Forward declarations: actions are handled from inside the scan poll loop too.
static bool pocTakeAction(PocWorker* w, u32* out_action);

// Pairing probe (defined next to the driver-level probe): the btm transport's
// end-of-window peek runs it while the connection is still up, which is the
// state the App pairs in. Accepting stays off - see the definition.
//
// Off since 2026-09-25 evening: the user's follow-up test shows the binding is
// enforced by the device itself (with binding on, cancelling the pairing prompt
// makes the App report "this device is already bound" and refuse to connect), so
// the device only ever pairs with the host it is being bound to and there is
// nothing for a third-party console to pair with. The code stays for the day the
// binding is cleared on a test device; on it costs up to 10s per call site.
#define POC_BTM_BOND_PROBE 0
#define POC_BTM_BOND_ACCEPT 0
static void pocBtdrvProbeBond(const BtdrvAddress* addr, const char* label);

// Raw `bt` service read (definition next to the transport): the service returns
// what libnx's wrappers throw away, and the CCCD experiment above uses it.
static void pocBtRawRead(u32 handle, u32 cmd, const BtdrvGattId* serv,
    const BtdrvGattId* chr, const BtdrvGattId* desc, const char* label);

static bool pocHandleAction(PocWorker* w, u32 action);
static u32 pocDrainBleEvents(const char* label, u32 duration_ms, u8* out_client_if);

// Automatic address discovery (definitions next to the advertisement dump): the
// driver-level scan writes the address out for the btm session that follows.
static bool pocAdIsCoyote(const BtdrvBleAdvertisement* list, u32 count);
static void pocSaveDiscoveredAddress(const BtdrvAddress* addr);

// Defined next to the btm probe, used by the driver-level probe's device dump.
static void pocLogAdStructures(const char* label, const u8* data, size_t size);
static void pocLogAdArray(const char* label, const BtdrvBleAdvertisement* list, u32 count);

// ---------------------------------------------------------------------------
// GATT operation results (notifications, read responses)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Session steps
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

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

// The btm probe's target address, so the end-of-window peek can run the pairing
// probe while the connection is still up - which is the state the App pairs in.
static BtdrvAddress g_btm_bond_address;
static bool g_btm_bond_address_valid;
static u8 g_managed_last[0x50];

// The other two queues btdrv keeps: the LE HID one and the general one. Both are
// big enough that they live in .bss like the managed payload.
static BtdrvBleEventInfo g_leh_event;
static u8 g_leh_last[0x50];
static BtdrvEventInfo g_general_event;

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

    // Every write would flood the ring, so the keepalive B0s are sampled (the
    // first few and every tenth after that). The packets that actually change
    // something are always worth a line: a strength packet is the one the device
    // is supposed to answer with B1, and the BF writes are the safety cap. The
    // sampling hid both in the 2026-09-25 15:58 round, where the output was
    // felt but the packet that asked for it never showed up in the log.
    bool carries_change = size == DGLAB_COYOTE_V3_BF_SIZE ||
        (size == DGLAB_COYOTE_V3_B0_SIZE && data[0] == DGLAB_COYOTE_V3_HEADER_B0 &&
            data[1] != 0u);

    if (carries_change || transport->writes < 3u || (transport->writes % 10u) == 0u)
        pocLog("btm transport: write %u byte(s) %s rc=0x%08X", (unsigned)size, hex, (u32)rc);

    // Byte 1 of a B0 is (sequence << 4) | A mode << 2 | B mode, the modes being
    // 0 no change, 1 relative +, 2 relative -, 3 absolute (docs/dglab-protocol.md).
    if (size == DGLAB_COYOTE_V3_B0_SIZE && data[0] == DGLAB_COYOTE_V3_HEADER_B0 &&
        data[1] != 0u) {
        pocLog("btm transport: strength seq=%u A mode=%u value=%u B mode=%u value=%u",
            (unsigned)(data[1] >> 4), (unsigned)((data[1] >> 2) & 0x3u), (unsigned)data[2],
            (unsigned)(data[1] & 0x3u), (unsigned)data[3]);
    }

    if (R_SUCCEEDED(rc))
        transport->writes++;
}

// The hardware-visible test, run after the zero-strength baseline. A strength
// request only opens the gate the device is allowed to use - the output itself
// is the waveform data, so a channel with strength and no waveform produces
// nothing whatsoever (user, 2026-09-25). The soft limit is enforced by the
// device itself, so even a garbled packet cannot push a channel past it.
//
// Set POC_BTM_TEST_STRENGTH to 0 to skip the test and keep the old "the device
// cannot output anything" behaviour.
#define POC_BTM_TEST_SOFT_LIMIT 20u
#define POC_BTM_TEST_STRENGTH 5u
#define POC_BTM_TEST_DURATION_MS 6000u

// The wheel phase tests the one strength change this console has nothing to do
// with: the device's own wheel. The official v3 README (example No.3) says a
// wheel change is answered with a B1 carrying sequence 0, and the user can feel
// the output rise - which proves the strength really did change. Neither half
// depends on our B0 being understood, so this is the cleanest test of the
// notification path that exists.
//
// Two conditions come out of the official example and the user's phone test
// (2026-09-25): the channel has to be outputting, so a waveform must play, and
// the soft limit is what keeps that output small. The device is brought to
// strength 0 by the baseline phase before this one starts.
//
// Set POC_BTM_WHEEL_DURATION_MS to 0 to skip the phase.
#define POC_BTM_WHEEL_SOFT_LIMIT 10u
#define POC_BTM_WHEEL_DURATION_MS 15000u

// How the probe subscribes to 0x150B. Both mechanisms were active together in
// v19/v20, RegisterNotification alone in v21, the hand written CCCD alone in
// v22 - all three silent. v24 goes back to RegisterNotification alone, which is
// the subscription the phone app's working session uses (the same API, the same
// stack), so the wheel phase compares like with like.
#define POC_BTM_NOTIFY_REGISTER 1
#define POC_BTM_CCCD_HAND_WRITE 0

// A slow up-and-down envelope. Four entries fill exactly one B0 packet, so the
// pattern repeats every 100ms and never parks at a high value.
static const DglabCoyoteV3WaveformEntry g_poc_btm_test_waveform[] = {
    { .frequency_ms = 100u, .strength = 0u },
    { .frequency_ms = 100u, .strength = 30u },
    { .frequency_ms = 100u, .strength = 60u },
    { .frequency_ms = 100u, .strength = 30u },
};

// A gentler envelope for the wheel phase: the wheel can push the channel up to
// the soft limit on its own, so the peak amplitude is halved to keep the ceiling
// at roughly what the reaction test reaches.
static const DglabCoyoteV3WaveformEntry g_poc_btm_wheel_waveform[] = {
    { .frequency_ms = 100u, .strength = 0u },
    { .frequency_ms = 100u, .strength = 15u },
    { .frequency_ms = 100u, .strength = 30u },
    { .frequency_ms = 100u, .strength = 15u },
};

// A GATT request that follows a read right away is answered with
// Bluetooth/0x153 on this firmware: both write types fail and the packet is
// dropped without a retry (2026-09-25, the two rounds that added the reads).
// The periodic B0 writes come back a tick later anyway, but the single-shot
// packets do not, so every read gets a moment of quiet before the next request.
static void pocBtmSettle(const char* reason)
{
    pocLog("btm transport: settle 300ms %s", reason);
    svcSleepThread(300000000ull);
}

// Reading through the `bt` service. libnx's read wrappers (bt.c cmd 0 = read
// characteristic, cmd 1 = read descriptor) pass no buffer and have no out
// parameter, so whatever the service returns is dropped on the floor - and the
// firmware's handlers do build a reply (btdrv's case 0x5a/0x5b copy the
// attribute value and id into a reply buffer after FUN_00077e70 succeeds). This
// issues the same CMIF command by hand with an out buffer attached, so the value
// is finally visible. A CCCD read of 0x0001 would answer the question this
// project has been stuck on: whether RegisterNotification / the hand-written
// descriptor write really reached the device.
static void pocBtRawRead(u32 handle, u32 cmd, const BtdrvGattId* serv, const BtdrvGattId* chr,
    const BtdrvGattId* desc, const char* label)
{
    struct {
        u8 is_primary;
        u8 auth_req;
        u8 pad[2];
        u32 connection_handle;
        BtdrvGattId serv_id;
        BtdrvGattId char_id;
        BtdrvGattId desc_id;
        u64 aruid;
    } in;
    u8 payload[0x200];
    u8 reply[0x40];
    Service service;
    Result rc;
    u32 i;

    memset(&in, 0, sizeof(in));
    memset(payload, 0, sizeof(payload));
    memset(reply, 0, sizeof(reply));

    in.is_primary = 1;
    in.connection_handle = handle;
    in.serv_id = *serv;
    in.char_id = *chr;
    if (desc != NULL)
        in.desc_id = *desc;
    in.aruid = appletGetAppletResourceUserId();

    rc = smGetService(&service, "bt");
    if (R_FAILED(rc)) {
        pocLog("%s: open 'bt' rc=0x%08X", label, (u32)rc);
        return;
    }

    rc = serviceDispatchInOut(&service, cmd, in, reply,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
        .buffers = { { payload, sizeof(payload) } },
        .in_send_pid = true);

    {
        char hex[3u * 0x20u + 1u];

        pocLog("%s: rc=0x%08X, reply (0x%02X bytes):", label, (u32)rc, (unsigned)sizeof(reply));
        pocLogWords(label, reply, sizeof(reply), 0u);
        pocHex(hex, sizeof(hex), payload, 0x20u);
        pocLog("%s: out %s", label, hex);
    }

    for (i = 0; i < 0x20u; i++) {
        if (payload[i] != 0)
            break;
    }

    pocLog("%s: payload nonzero=%u", label, (unsigned)(i < 0x20u));
    serviceClose(&service);
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

    if (POC_BTM_NOTIFY_REGISTER) {
        rc = btLeClientRegisterNotification(handle, true, &g_btm_transport.service,
            &g_btm_transport.notify_char);
        pocLog("btm transport: RegisterNotification(0x150B) rc=0x%08X", (u32)rc);
        g_btm_transport.notify_registered = R_SUCCEEDED(rc);
    } else {
        pocLog("btm transport: RegisterNotification off (A/B: hand written CCCD only)");
    }

    // RegisterNotification only says "accepted". If it really subscribed, the
    // CCCD under 0x150B reads back as 0x0001; if it did not, the device has no
    // reason to ever notify and the whole B1 wait is pointless. The answer
    // arrives through the same event channel as everything else.
    if (g_btm_cccd_ready) {
        Result cccd_rc = btLeClientReadDescriptor(handle, true, &g_btm_transport.service,
            &g_btm_transport.notify_char, &g_btm_cccd, BtdrvGattAuthReqType_None);

        pocLog("btm transport: ReadDescriptor(CCCD 0x2902 id=%u) rc=0x%08X",
            (unsigned)g_btm_cccd.instance_id, (u32)cccd_rc);
        pocBtmSettle("after the CCCD read");
    } else {
        pocLog("btm transport: no CCCD entry from btm, subscription cannot be read back");
    }

    // Writing 0x0001 into the CCCD under 0x150B is a subscription done by hand.
    // It is off by default now (see POC_BTM_CCCD_HAND_WRITE): v19/v20 had both
    // ways active and got nothing back, and a hand-written CCCD can disagree
    // with the stack's own idea of the subscription, so the next round lets
    // RegisterNotification own it. Turn it on to test the other half.
    if (POC_BTM_CCCD_HAND_WRITE && g_btm_cccd_ready) {
        u8 cccd[2] = { 0x01, 0x00 };
        Result cccd_write = btLeClientWriteDescriptor(handle, true,
            &g_btm_transport.service, &g_btm_transport.notify_char, &g_btm_cccd,
            cccd, sizeof(cccd), BtdrvGattAuthReqType_None);

        pocLog("btm transport: WriteDescriptor(CCCD 0x2902 = 0100) rc=0x%08X",
            (u32)cccd_write);
        pocBtmSettle("after the CCCD write");
    } else {
        pocLog("btm transport: CCCD hand write off, RegisterNotification owns the subscription");
    }

    // Ask the service for the values libnx drops (see pocBtRawRead). The CCCD
    // right after subscribing is the one that matters: 0x0001 means the
    // subscription really is in place and the device has a reason to notify.
    pocBtmSettle("before the raw reads");

    // The battery first: its value is a known quantity (a percentage the device
    // has to report), so it calibrates what a read reply looks like before the
    // CCCD read needs to be interpreted.
    if (g_btm_battery_ready) {
        BtdrvGattId battery_service;
        BtdrvGattId battery_char;

        memset(&battery_service, 0, sizeof(battery_service));
        battery_service.instance_id = (u8)g_btm_battery_service.instance_id;
        battery_service.uuid = g_btm_battery_service.uuid;
        memset(&battery_char, 0, sizeof(battery_char));
        battery_char.instance_id = (u8)g_btm_battery_char.instance_id;
        battery_char.uuid = g_btm_battery_char.uuid;

        pocBtRawRead(handle, 0u, &battery_service, &battery_char, NULL,
            "btm transport: raw battery");
        pocBtmSettle("after the raw battery read");
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
                pocBtmSettle("after the battery read");
            }
        }

        svcSleepThread(10000000ull); // 10ms
    }
}

// Dumps one of btdrv's BLE queues: the queue hands the same record back until
// something else arrives, so only a record that differs from the previous read
// is worth a line. 0x50 bytes are compared and dumped, which is where a
// notification payload would sit (size at +0x48, payload at +0x4A).
static u32 pocBtmPeekBleQueue(const char* label,
    Result (*read)(void*, size_t, BtdrvBleEventType*), BtdrvBleEventInfo* event, u8* last)
{
    u32 seen = 0u;

    for (u32 i = 0; i < 8u; i++) {
        BtdrvBleEventType type = (BtdrvBleEventType)0;
        bool nonzero = false;
        Result rc;

        memset(event, 0, sizeof(*event));
        rc = read(event, sizeof(*event), &type);
        if (R_FAILED(rc)) {
            pocLog("btm transport: %s peek read rc=0x%08X", label, (u32)rc);
            break;
        }

        for (u32 b = 0; b < sizeof(g_managed_last); b++) {
            if (event->data[b] != 0)
                nonzero = true;
        }

        if (!nonzero)
            break;

        if (memcmp(last, event->data, sizeof(g_managed_last)) == 0)
            continue;

        memcpy(last, event->data, sizeof(g_managed_last));
        seen++;

        if (seen <= 4u) {
            char raw_label[48];

            snprintf(raw_label, sizeof(raw_label), "btm transport: %s#%u type=%u",
                label, (unsigned)seen, (unsigned)type);
            pocLogWords(raw_label, event->data, 0x50u, 0u);
        }
    }

    pocLog("btm transport: %s peek done, %u distinct record(s)", label, (unsigned)seen);
    return seen;
}

// Last thing before the disconnect: do btdrv's own queues carry the records the
// `bt` channel never shows? A notification that never reaches us has to be
// somewhere, and btdrv keeps three states: the managed queue, the LE HID one and
// the general one (btdrvGetEventInfo, the one btm itself reads). This probe has
// stayed away from btdrv on purpose (docs/history.md §28) - what broke the
// connect was a second InitializeBle/EnableBle and a RegisterGattClient from
// this process, not opening the service - so the peek runs after the transport
// window, when the measurement is already in the log, and it only reads.
static void pocBtmEventPeek(void)
{
    Result rc = btdrvInitialize();

    pocLog("btm transport: peek, btdrvInitialize rc=0x%08X", (u32)rc);
    if (R_FAILED(rc))
        return;

    // Pairing probe first: it watches the general queue for a pairing request,
    // and that queue is also one of the three dumped below.
    if (POC_BTM_BOND_PROBE && g_btm_bond_address_valid)
        pocBtdrvProbeBond(&g_btm_bond_address, "btm transport");

    pocBtmPeekBleQueue("managed", btdrvGetBleManagedEventInfo, &g_managed_event, g_managed_last);
    pocBtmPeekBleQueue("lehid", btdrvGetLeHidEventInfo, &g_leh_event, g_leh_last);

    // The general queue uses its own payload struct and type enum, so it gets
    // its own small block. btm reads this queue too, so an empty read here is a
    // possible outcome; it is still worth a line.
    {
        const u32 dump = sizeof(g_general_event) < 0x50u ? (u32)sizeof(g_general_event) : 0x50u;
        const u8* raw = (const u8*)&g_general_event;
        BtdrvEventType type = (BtdrvEventType)0;
        bool nonzero = false;

        memset(&g_general_event, 0, sizeof(g_general_event));
        rc = btdrvGetEventInfo(&g_general_event, sizeof(g_general_event), &type);
        if (R_FAILED(rc)) {
            pocLog("btm transport: general peek read rc=0x%08X", (u32)rc);
        } else {
            for (u32 b = 0; b < dump; b++) {
                if (raw[b] != 0)
                    nonzero = true;
            }

            if (nonzero) {
                char raw_label[48];

                snprintf(raw_label, sizeof(raw_label), "btm transport: general#1 type=%u",
                    (unsigned)type);
                pocLogWords(raw_label, raw, dump, 0u);
            } else {
                pocLog("btm transport: general peek empty (type=%u)", (unsigned)type);
            }
        }
    }

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

    pocBtmEventPeek();

    pocLog("btm transport: done, writes=%u notify=%u b1=%u", g_btm_transport.writes,
        g_btm_transport.notifications, g_btm_transport.b1_count);
    g_btm_transport.connected = false;
}

// Re-announces BF with the given soft limit on both channels. OnConnected also
// resets the strength bookkeeping and the waveform playback position, which is
// what a phase change wants; the configured waveform data itself is kept.
static void pocBtmTransportSetSoftLimits(u8 limit)
{
    g_btm_transport.session.bf.soft_limit_a = limit;
    g_btm_transport.session.bf.soft_limit_b = limit;
    dglabCoyoteV3SessionOnConnected(&g_btm_transport.session);
}

// Strength opens the gate, the waveform is the output: this is the only phase
// whose result does not depend on a packet coming back, so it is the only phase
// that can tell "the device ignores us" from "we never reach the device". It is
// also the only phase where the device is allowed to output anything at all,
// which is why the limits are tiny and are put back to zero before the
// disconnect (see POC_BTM_TEST_* above).
static void pocBtmTransportReactionTest(void)
{
    const size_t entries = sizeof(g_poc_btm_test_waveform) / sizeof(g_poc_btm_test_waveform[0]);

    if (!g_btm_transport.connected)
        return;

    if (POC_BTM_TEST_STRENGTH == 0u) {
        pocLog("btm transport: reaction test skipped (POC_BTM_TEST_STRENGTH is 0)");
        return;
    }

    pocLog("btm transport: reaction test soft=%u strength=%u peak=%u for %ums",
        (unsigned)POC_BTM_TEST_SOFT_LIMIT, (unsigned)POC_BTM_TEST_STRENGTH,
        (unsigned)g_poc_btm_test_waveform[2].strength, (unsigned)POC_BTM_TEST_DURATION_MS);

    pocBtmTransportSetSoftLimits((u8)POC_BTM_TEST_SOFT_LIMIT);

    if (!dglabCoyoteV3SessionSetWaveform(&g_btm_transport.session, DglabCoyoteV3ChannelA,
            g_poc_btm_test_waveform, entries)) {
        pocLog("btm transport: reaction test waveform rejected, capping again");
        pocBtmTransportSetSoftLimits(0u);
        return;
    }

    // Channel B stays idle: one channel is enough to see whether the device
    // reacts at all.
    dglabCoyoteV3SessionAdjustStrength(&g_btm_transport.session, DglabCoyoteV3ChannelA,
        (int32_t)POC_BTM_TEST_STRENGTH);

    pocBtmTransportPump(POC_BTM_TEST_DURATION_MS);

    // Teardown, in this order: cap the device first (BF 0 makes everything that
    // follows harmless), then stop the waveform and ask for an absolute zero,
    // then give the zero one more second so it actually goes out before the
    // disconnect.
    pocLog("btm transport: reaction test done, capping and zeroing");
    pocBtmTransportSetSoftLimits(0u);
    dglabCoyoteV3SessionClearWaveform(&g_btm_transport.session, DglabCoyoteV3ChannelA);
    dglabCoyoteV3SessionSetStrengthZero(&g_btm_transport.session, DglabCoyoteV3ChannelA);
    dglabCoyoteV3SessionSetStrengthZero(&g_btm_transport.session, DglabCoyoteV3ChannelB);
    pocBtmTransportPump(1000u);
}

// The device changes its own strength while this phase runs - the user turns the
// wheel - and per the official README that must answer a B1 with sequence 0. The
// console sends nothing but the waveform keepalive here (every B0 carries "no
// change" for both channels), so anything that arrives in this window came from
// the device's own control.
static void pocBtmTransportWheelPhase(void)
{
    const size_t entries = sizeof(g_poc_btm_wheel_waveform) / sizeof(g_poc_btm_wheel_waveform[0]);

    if (!g_btm_transport.connected)
        return;

    if (POC_BTM_WHEEL_DURATION_MS == 0u) {
        pocLog("btm transport: wheel phase skipped (POC_BTM_WHEEL_DURATION_MS is 0)");
        return;
    }

    pocLog("btm transport: wheel phase soft=%u peak=%u for %ums - TURN THE WHEEL NOW",
        (unsigned)POC_BTM_WHEEL_SOFT_LIMIT, (unsigned)g_poc_btm_wheel_waveform[2].strength,
        (unsigned)POC_BTM_WHEEL_DURATION_MS);

    pocBtmTransportSetSoftLimits((u8)POC_BTM_WHEEL_SOFT_LIMIT);

    if (!dglabCoyoteV3SessionSetWaveform(&g_btm_transport.session, DglabCoyoteV3ChannelA,
            g_poc_btm_wheel_waveform, entries)) {
        pocLog("btm transport: wheel phase waveform rejected, capping again");
        pocBtmTransportSetSoftLimits(0u);
        return;
    }

    pocBtmTransportPump(POC_BTM_WHEEL_DURATION_MS);

    pocLog("btm transport: wheel phase done, capping and zeroing");
    pocBtmTransportSetSoftLimits(0u);
    dglabCoyoteV3SessionClearWaveform(&g_btm_transport.session, DglabCoyoteV3ChannelA);
    dglabCoyoteV3SessionSetStrengthZero(&g_btm_transport.session, DglabCoyoteV3ChannelA);
    dglabCoyoteV3SessionSetStrengthZero(&g_btm_transport.session, DglabCoyoteV3ChannelB);
    pocBtmTransportPump(1000u);
}

// One transport round: the zero-strength baseline first (soft limits 0, nothing
// can come out of the device, B0 packets still carry sequence numbers the device
// is supposed to answer), then the wheel phase (a change the device makes on its
// own) and the reaction test (a change we make).
static void pocBtmTransportRun(u32 handle)
{
    if (!pocBtmTransportStart(handle))
        return;

    pocBtmTransportPump(3000u);
    pocBtmTransportWheelPhase();
    pocBtmTransportReactionTest();
    pocBtmTransportStop();
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

// A connect request with the deliberately invalid client_if 0xFF: it can never
// succeed, which is the point - it answers "is the stack reachable at all from
// this session" without depending on anything else.
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

// Pairing probe. The device only offers pairing while its own App-driven
// "device binding" mode is on (on iOS the toggle makes the phone show a pairing
// prompt; with it off the connection simply stays transient), so a run of this
// only means something if that mode was enabled beforehand. The request arrives
// on the *general* btdrv event queue (SspRequest = 3, PairingPinCodeRequest = 2)
// and nothing else on this console will answer it - so the probe answers by
// cancelling (POC_BTM_BOND_ACCEPT is declared with the forward declarations,
// next to the other tunables).
static void pocBtdrvProbeBond(const BtdrvAddress* addr, const char* label)
{
    SetSysBluetoothDevicesSettings settings;
    Result rc = btdrvCreateBond(*addr, 0);

    pocLog("%s: CreateBond(type=0) rc=0x%08X", label, (u32)rc);

    for (u32 i = 0; i < 40u; i++) {
        BtdrvEventType type = (BtdrvEventType)0;
        bool answered = false;

        memset(&g_general_event, 0, sizeof(g_general_event));
        rc = btdrvGetEventInfo(&g_general_event, sizeof(g_general_event), &type);

        if (R_SUCCEEDED(rc)) {
            const u8* raw = (const u8*)&g_general_event;

            if ((u32)type == (u32)BtdrvEventType_SspRequest ||
                (u32)type == (u32)BtdrvEventType_PairingPinCodeRequest) {
                Result answer;

                pocLog("%s: pairing event type=%u from %02X:%02X:...:%02X", label, (u32)type,
                    raw[0], raw[1], raw[5]);

                if (POC_BTM_BOND_ACCEPT) {
                    answer = btdrvRespondToSspRequest(*addr, 0, true, 0);
                    pocLog("%s: RespondToSspRequest(accept=1) rc=0x%08X", label, (u32)answer);
                } else {
                    answer = btdrvCancelBond(*addr);
                    pocLog("%s: bond canceled (accept=0) rc=0x%08X", label, (u32)answer);
                }

                answered = true;
            }
        }

        if (answered)
            break;

        svcSleepThread(250000000ull); // 250ms, up to 10s
    }

    memset(&settings, 0, sizeof(settings));
    rc = btdrvGetPairedDeviceInfo(*addr, &settings);
    pocLog("%s: paired readback rc=0x%08X link_key_present=%u", label, (u32)rc,
        (u32)settings.link_key_present);
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

    // Pairing probe (see pocBtdrvProbeBond): only meaningful when the device's
    // "device binding" mode was enabled in the App first.
    if (POC_BTM_BOND_PROBE && g_poc.use_target_address) {
        BtdrvAddress bond_address;

        memset(&bond_address, 0, sizeof(bond_address));
        memcpy(bond_address.address, g_poc.target_address, sizeof(bond_address.address));
        pocBtdrvProbeBond(&bond_address, "btdrv probe");
    }

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

                    // No address configured yet? Recognise the device from its
                    // advertisement and write it out for the btm session that
                    // follows (see pocSaveDiscoveredAddress).
                    if (!g_poc.use_target_address && !g_poc.address_discovered &&
                        pocAdIsCoyote(info.scan_result.ad_list, 10u)) {
                        g_poc.address_discovered = true;
                        pocSaveDiscoveredAddress(&info.scan_result.address);
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
// Automatic address discovery. The driver-level scan is the only scan that ever
// sees this device (btm's own scans have never returned a result), and a connect
// needs an address, so the console should not have to be told one by hand: when a
// scan result carries the device's advertisement (local name "47L121000" or
// manufacturer data from company 0x000A) and no address is configured yet, that
// address is written to the same file the NRO reads before every session - which
// means the btm session of the same one-key sequence picks it up on its own.
#define POC_ADDRESS_PATH "sdmc:/switch/DGLAB-NX/config/dglab-ble-address.txt"

static bool pocAdIsCoyote(const BtdrvBleAdvertisement* list, u32 count)
{
    static const char name[] = "47L121000";

    for (u32 i = 0; i < count; i++) {
        if (list[i].size < 2u || list[i].size > (1u + sizeof(list[i].data)))
            break;

        if (list[i].type == 0x09u && list[i].size - 1u >= sizeof(name) - 1u &&
            memcmp(list[i].data, name, sizeof(name) - 1u) == 0)
            return true;

        if (list[i].type == 0xFFu && list[i].size >= 3u && list[i].data[0] == 0x0Au &&
            list[i].data[1] == 0x00u)
            return true;
    }

    return false;
}

static void pocSaveDiscoveredAddress(const BtdrvAddress* addr)
{
    FILE* file;

    fsInitialize();
    fsdevMountSdmc();

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/DGLAB-NX", 0777);
    mkdir("sdmc:/switch/DGLAB-NX/config", 0777);

    file = fopen(POC_ADDRESS_PATH, "w");

    if (file == NULL) {
        pocLog("btdrv probe: device %02X:%02X:...:%02X but %s is not writable",
            addr->address[0], addr->address[1], addr->address[5], POC_ADDRESS_PATH);
        return;
    }

    fprintf(file, "%02X:%02X:%02X:%02X:%02X:%02X\n", addr->address[0], addr->address[1],
        addr->address[2], addr->address[3], addr->address[4], addr->address[5]);
    fclose(file);

    pocLog("btdrv probe: discovered the device at %02X:%02X:%02X:%02X:%02X:%02X, saved to %s",
        addr->address[0], addr->address[1], addr->address[2], addr->address[3],
        addr->address[4], addr->address[5], POC_ADDRESS_PATH);
}

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
            // layer: BF + a B0 stream and the B1 answers, which is what a real
            // transport has to do, then the reaction test (see pocBtmTransportRun).
            if (g_btm_proto_ready) {
                g_btm_bond_address = address;
                g_btm_bond_address_valid = true;
                pocBtmTransportRun(handle);
                g_btm_bond_address_valid = false;
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
                g_btm_bond_address = configured;
                g_btm_bond_address_valid = true;
                pocBtmTransportRun(handle);
                g_btm_bond_address_valid = false;
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

// There are exactly two probes left, and each one asks for a session of its own:
// the driver-level probe is the only thing that may bring the BLE stack up, and
// the base-btm probe needs a console this project has not touched yet. The flag
// is set here and the thread runs the probe, so a probe never starts from inside
// an action handler.
static bool pocHandleAction(PocWorker* w, u32 action)
{
    switch (action) {
        case DglabPocAction_ProbeBtdrvScan:
            pocLog("action: btdrv scan probe");
            w->probe_btdrv = true;
            return true;

        case DglabPocAction_ProbeBtmBle:
            pocLog("action: base btm BLE probe");
            w->probe_btm = true;
            return true;

        default:
            pocLog("action: %u is not a probe this build knows", (unsigned)action);
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
// Worker thread
// ---------------------------------------------------------------------------

static void pocThreadFunc(void* arg)
{
    PocWorker* w = &g_poc.worker;
    u32 action;

    (void)arg;

    memset(w, 0, sizeof(*w));

    pocSetState(DglabPocState_Initializing);
    pocLog("poc start aruid_low=0x%08X", (u32)g_poc.aruid);
    // Printed by every session so a log says which sysmodule build produced it;
    // the probe versions below only appear when their key is pressed.
    pocLog("poc build: ble_poc v34 (automatic address discovery)");

    // The NRO sends START and the first ACTION back to back, so give that action
    // a moment to arrive before any probe runs: both probes care about what has
    // touched Bluetooth before them.
    svcSleepThread(300000000ull); // 300ms

    while (pocTakeAction(w, &action))
        pocHandleAction(w, action);

    // The base-btm probe owns the whole session: it must see the console the way
    // it is before this project touched Bluetooth (btm and the BLE manager keep
    // per-session state, docs/ble-poc.md), and there is nothing for the scan
    // loop to add to a btm answer. So it runs first, and the session ends here.
    if (w->probe_btm) {
        w->probe_btm = false;
        pocRunBtmBleProbe(w);
    }

    // The driver-level probe runs in a session of its own as well: it is the one
    // that calls InitializeBle/EnableBle, and the stack must not be brought up
    // from inside the session that talks to btm (that combination is what made
    // the connect fail, docs/history.md §28). It gets a session because the
    // console's one-key sequence asks for it first, and because it is still the
    // only way to see the advertisement with the driver in control.
    if (w->probe_btdrv) {
        w->probe_btdrv = false;
        pocRunBtdrvScanProbe(w);
    }

    // What used to be here - the btdev scan/connect/discover/subscribe route and
    // the two ARUID/identity diagnostics - is gone. The btdev route could never
    // connect on this firmware (the four GATT client slots belong to btm,
    // docs/ble-re.md), the identity probe's question (do libnx's btdrv command
    // numbers still match the firmware's) is settled, and the btm:u path is
    // applet-only. docs/history.md keeps the record of all of them.
    pocLog("poc session: no probe requested, nothing to do");

    mutexLock(&g_poc.mutex);
    bool failed = g_poc.status.state == DglabPocState_Failed;

    if (!failed)
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
    g_poc.status.state = DglabPocState_Idle;
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
    g_poc.pending_action = 0;
    g_poc.stop_requested = false;
    g_poc.aruid = request->applet_resource_user_id;
    g_poc.status.aruid_low = (u32)g_poc.aruid;
    g_poc.use_target_address = (request->flags & DGLAB_POC_START_FLAG_TARGET_ADDRESS) != 0;
    memcpy(g_poc.target_address, request->target_address, sizeof(g_poc.target_address));
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
