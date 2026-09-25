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
    DglabPocState_Initializing,  ///< A session was started, the probe has not run yet.
    DglabPocState_Failed,        ///< Stopped because of an error, see last_result.
    DglabPocState_Stopped,       ///< Stopped on request.
} DglabPocState;

// Milestones reached during a probe. A bit is set once the step succeeded, so a
// single status read shows how far the run got.
#define DGLAB_POC_MILESTONE_DEVICE_FOUND  (1u << 0)
#define DGLAB_POC_MILESTONE_CONNECTED     (1u << 1)
#define DGLAB_POC_MILESTONE_SERVICE_FOUND (1u << 2)

// The console has exactly two actions left. Each one runs a whole session of its
// own, in this order: the driver-level probe first (it is what brings the BLE
// stack up - InitializeBle/EnableBle must not run in the session that talks to
// btm, docs/history.md §28), then the base `btm` probe (scan, connect, GATT
// table, transport). Everything else the console used to offer - the btdev
// scan/connect route, the btdrv identity probe, the btm:u ARUID probes and the
// scan-filter variants - was removed once the answers were in; docs/history.md
// keeps the record.
enum {
    DglabPocAction_ProbeBtdrvScan = 1, ///< Driver-level: bring the BLE stack up, scan from btdrv, dump the advertisement.
    DglabPocAction_ProbeBtmBle    = 2, ///< Base `btm`: scan, connect, GATT table, transport (BF/B0 + reaction test).
};

// Direct connect: skip the scan entirely and connect to this address. Useful
// because btm's scan filters are system configured and may not match a generic
// BLE peripheral.
#define DGLAB_POC_START_FLAG_TARGET_ADDRESS (1u << 0)

typedef struct {
    u64 applet_resource_user_id;
    u32 flags;            ///< DGLAB_POC_START_FLAG_*
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
} DglabPocStatus;

typedef struct {
    u32 next_cursor;
    u32 size;
    char text[DGLAB_POC_LOG_CHUNK_SIZE];
} DglabPocLogChunk;
