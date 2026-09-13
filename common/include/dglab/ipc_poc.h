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
    DglabPocAction_ReconnectAruid0  = 5, ///< Retry the connection with AppletResourceUserId 0.
    DglabPocAction_ToggleAutoWrite  = 6, ///< Toggle the 100ms B0 keepalive while connected.
    // Diagnostics for the case where the scan reports no usable result:
    DglabPocAction_RescanNoFilter   = 7, ///< Clear the scan filters, disable filtering and scan again.
    DglabPocAction_ConnectLastScan  = 8, ///< Connect to the last scanned address even if the advertisement did not match.
    DglabPocAction_ProbeBtdev       = 9, ///< Run the scan through libnx's btdev (bt/btm:u) wrapper instead of btdrv.
};

typedef struct {
    u64 applet_resource_user_id;
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
