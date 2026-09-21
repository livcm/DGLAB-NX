#pragma once

#include <switch/types.h>

// TEMPORARY IPC surface for the BLE transport PoC.
//
// These commands exist to drive and observe the BLE proof of concept from the
// NRO while the real transport is still being validated on hardware. They are
// not part of the stable IPC contract in dglab/ipc.h and are expected to be
// removed once the transport works and its real commands (connect, set
// strength, send waveform) are designed.
//
// Command ids are kept in a high range so they can never collide with the
// stable commands.

enum {
    DGLAB_IPC_POC_CMD_START  = 0x80, // in: DglabPocStartRequest
    DGLAB_IPC_POC_CMD_STOP   = 0x81, // no payload
    DGLAB_IPC_POC_CMD_STATUS = 0x82, // out: DglabPocStatus
    DGLAB_IPC_POC_CMD_LOG    = 0x83, // in: DglabPocLogRequest, out: DglabPocLogChunk
    DGLAB_IPC_POC_CMD_ACTION = 0x84, // in: DglabPocActionRequest
};

// Kept small on purpose: a CMIF reply that uses the inline data area has to fit
// next to the CMIF header inside the 0x100-byte IPC buffer. The reader just asks
// for the next chunk until it has caught up.
#define DGLAB_POC_LOG_CHUNK_SIZE 160u

typedef enum {
    DglabPocState_Idle = 0,      ///< Nothing running.
    DglabPocState_Initializing,  ///< Acquiring the BLE event and enabling BLE.
    DglabPocState_Scanning,      ///< Scanning for the Coyote 3.0.
    DglabPocState_Registering,   ///< Registering the GATT client.
    DglabPocState_Connecting,    ///< Connecting to the GATT server.
    DglabPocState_Discovering,   ///< Resolving services and characteristics.
    DglabPocState_Ready,         ///< Connected, subscribing, writing B0.
    DglabPocState_Failed,        ///< Stopped because of an error, see last_result.
    DglabPocState_Stopped,       ///< Stopped on request.
} DglabPocState;

// Milestones reached during the PoC. A bit is set once the step succeeded, so a
// single status read shows how far the run got.
#define DGLAB_POC_MILESTONE_BLE_READY     (1u << 0)
#define DGLAB_POC_MILESTONE_SCAN_STARTED  (1u << 1)
#define DGLAB_POC_MILESTONE_DEVICE_FOUND  (1u << 2)
#define DGLAB_POC_MILESTONE_CLIENT_READY  (1u << 3)
#define DGLAB_POC_MILESTONE_CONNECTED     (1u << 4)
#define DGLAB_POC_MILESTONE_SERVICE_FOUND (1u << 5)
#define DGLAB_POC_MILESTONE_CHARS_FOUND   (1u << 6)
#define DGLAB_POC_MILESTONE_NOTIFY_ON     (1u << 7)
#define DGLAB_POC_MILESTONE_B0_WRITTEN    (1u << 8)
#define DGLAB_POC_MILESTONE_B1_RECEIVED   (1u << 9)
#define DGLAB_POC_MILESTONE_BATTERY_READ  (1u << 10)

enum {
    DglabPocAction_Disconnect       = 1, ///< Disconnect and go back to idle.
    DglabPocAction_WriteIdleB0      = 2, ///< One harmless B0 write (no strength change).
    DglabPocAction_WriteZeroB0      = 3, ///< B0 that sets both channels to strength 0, so the device answers with B1.
    DglabPocAction_ReadBattery      = 4, ///< Read the battery characteristic.
    DglabPocAction_RestartSession   = 5, ///< Tear the session down and start over.
    DglabPocAction_ToggleAutoWrite  = 6, ///< Toggle the 100ms B0 keepalive while connected.
    // Scan variants. The BLE advertisement of the Coyote 3.0 carries the HID
    // service UUID 0x1812, not the DG-LAB service 0x180C, so the scan filter
    // decides whether the device can be found at all.
    DglabPocAction_Rescan              = 7,  ///< Scan again with the default filter order.
    DglabPocAction_ScanWithProtocolUuid = 8, ///< Scan filtered by 0x180C only.
    DglabPocAction_ScanWithAdvertisedUuid = 9, ///< Scan filtered by 0x1812 only.
    DglabPocAction_ScanWithGeneralFilter = 10, ///< Scan using btm's general (manufacturer data) filter.
    DglabPocAction_ProbeBtdrvScan = 11, ///< Last driver-level attempt: set scan parameters and poll btdrv's BLE event queue.
    // Identity probe: read the local adapter's own properties and the BLE
    // channel map, then run the GATT client register/unregister sequence.
    // Those reads are self-validating (a name, a MAC, a channel bitmap), which
    // is what decides whether libnx's btdrv command numbers still match the
    // firmware's: a drifted number returns success with nothing usable.
    DglabPocAction_ProbeBtdrvIdentity = 12,
    // Positive control for "does btm's scan machinery run for this process at
    // all": btm's general scan is a manufacturer-data filter and the stored
    // value is Nintendo's company ID, which matches nothing in a normal room.
    // Each press scans for the next company ID that phones and earbuds actually
    // advertise. Only a hit is a verdict; a miss is not.
    DglabPocAction_ScanWithCommonCompany = 13,
};

// Direct connect: skip the scan entirely and connect to this address. Useful
// because btm's scan filters are system configured and may not match a generic
// BLE peripheral.
#define DGLAB_POC_START_FLAG_TARGET_ADDRESS (1u << 0)

// Start without the diagnostic probes. The BLE manager binds its internal
// connection to the session that first initialized it, and the identity probe
// (and the driver-level probe) both touch that state; a session started for a
// plain scan therefore has to be able to opt out, otherwise "does btm scan for
// this process at all" can never be observed on a clean console. See
// docs/ble-re.md.
#define DGLAB_POC_START_FLAG_SKIP_PROBES (1u << 1)

typedef struct {
    u64 applet_resource_user_id;
    u32 flags;            ///< DGLAB_POC_START_FLAG_*
    u32 scan_filter;      ///< 0 = default order, otherwise a 16-bit UUID or 0xFFFF for the general filter
    u8 target_address[6]; ///< Used with DGLAB_POC_START_FLAG_TARGET_ADDRESS
    u8 pad[2];
} DglabPocStartRequest;

typedef struct {
    u32 action;
} DglabPocActionRequest;

typedef struct {
    u32 cursor;
} DglabPocLogRequest;

typedef struct {
    u32 state;       ///< DglabPocState
    u32 milestone;   ///< DGLAB_POC_MILESTONE_* bitmask
    u32 last_result; ///< Result of the last failing step, 0 when none.
    u32 aruid_low;   ///< Low 32 bits of the AppletResourceUserId used for the connection.

    u32 client_if;
    u32 conn_id;
    u32 scan_results; ///< Scan result events seen.
    u32 scan_matched; ///< Results that matched the Coyote 3.0.
    u32 notify_count; ///< BLE notification events received.
    u32 b0_write_count;
    u32 b0_write_failures;
    u32 auto_write;   ///< Whether the 100ms keepalive is enabled.

    u32 battery_value;
    u32 battery_valid;
    u32 last_notify_size;
    u32 mtu;
    u32 event_count;     ///< BLE events received from btdrv, any type.
    u32 last_event_type; ///< BtdrvBleEventType of the last event.
    u32 filter_used;     ///< Service UUID the device was found with, 0 when not found.

    u8 address[6];
    u8 address_valid;
    u8 ble_addr_type;
    u8 char_write_prop;
    u8 char_notify_prop;
    u8 char_battery_prop;
    u8 pad[2];

    u8 last_notify[20];
} DglabPocStatus;

typedef struct {
    u32 next_cursor;
    u32 size;
    char text[DGLAB_POC_LOG_CHUNK_SIZE];
} DglabPocLogChunk;
