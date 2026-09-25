#pragma once

#include <switch/types.h>

// The service name is limited to 8 characters by the Switch service manager.
#define DGLAB_IPC_SERVICE_NAME "dglab"

// IPC protocol version, packed as 0xMMmmpp. DGLAB_IPC_CMD_GET_VERSION reports
// these three bytes, so this constant is the single source for the number a
// client sees: increment the minor (or major) whenever the IPC surface changes
// in a way that is not backward compatible.
//
// This is the interface version, not the application's release version (that
// one lives in the NRO's NACP, see AGENTS.md priority 13).
#define DGLAB_IPC_PROTOCOL_VERSION 0x000202u

// Commands implemented by the sysmodule. Command IDs are part of the public IPC
// contract and must not be renumbered once released.
enum {
    DGLAB_IPC_CMD_GET_VERSION = 0,
    DGLAB_IPC_CMD_PING        = 1,
    DGLAB_IPC_CMD_NET_START   = 2,
    DGLAB_IPC_CMD_NET_STOP    = 3,
    DGLAB_IPC_CMD_NET_STATUS  = 4,
    DGLAB_IPC_CMD_NET_QR      = 5,
    DGLAB_IPC_CMD_NET_SEND    = 6,
    DGLAB_IPC_CMD_NET_LOG     = 7,
    DGLAB_IPC_CMD_NET_WAVEFORM = 8,
    // BLE mode (sysmodule drives the device itself). BLE_START takes a soft
    // limit - the ceiling the device enforces - so "no output" is what a client
    // gets unless it asks for more. Strength and waveform arrive through the
    // same two commands the Socket mode uses, so a client's gameplay code does
    // not change when it switches transport.
    DGLAB_IPC_CMD_BLE_START   = 9,  // in: DglabBleStartRequest
    DGLAB_IPC_CMD_BLE_STOP    = 10, // no payload
    DGLAB_IPC_CMD_BLE_STATUS  = 11, // out: DglabBleStatus
    // Changes the ceiling while the session runs. The keys on the page are live,
    // so a client does not have to stop and start the session (which would drop
    // the link and, on this firmware, spend another BLE path) just to move it.
    // Without a running session there is nothing to move: the start request
    // carries the value.
    DGLAB_IPC_CMD_BLE_LIMIT   = 12, // in: DglabBleLimitRequest
};

// Value returned by DGLAB_IPC_CMD_PING. Keeping this stable gives clients a
// cheap way to verify that they are talking to the right service.
#define DGLAB_IPC_PING_MAGIC 0x44474C42u

// Version information returned by DGLAB_IPC_CMD_GET_VERSION.
typedef struct DglabIpcVersion {
    u32 major;
    u32 minor;
    u32 patch;
} DglabIpcVersion;

// ---------------------------------------------------------------------------
// Wi-Fi + WebSocket transport (DG-LAB Socket V3)
//
// The sysmodule runs the WebSocket server the phone App scans a QR code for,
// and acts as the controller on the same server. See docs/dglab-socket.md for
// the protocol and docs/ipc.md for how these commands are used.
// ---------------------------------------------------------------------------

/// Default port of the reference V3 server.
#define DGLAB_NET_DEFAULT_PORT 9999u

/// Client ids are uuid strings; the array keeps the terminator.
#define DGLAB_NET_ID_LEN 40u

/// Value of DglabNetStatus::app_feedback while no feedback button was pressed.
#define DGLAB_NET_FEEDBACK_NONE 0xFFu

typedef enum {
    DglabNetState_Idle = 0,  ///< Not started.
    DglabNetState_Listening, ///< Listening, waiting for the App to scan the QR code.
    DglabNetState_Paired,    ///< The App is bound; commands can be sent.
    DglabNetState_Stopped,   ///< Stopped on request.
    DglabNetState_Failed,    ///< Start failed, see DglabNetStatus::last_result.
} DglabNetState;

/// Commands a client can ask the sysmodule to send to the bound App.
typedef enum {
    DglabNetCommand_SetStrength = 1, ///< value = strength 0..200, channel 0 = both
    DglabNetCommand_Clear       = 2, ///< Clear the waveform queue, channel 0 = both
    DglabNetCommand_TestPulse   = 3, ///< Built in test waveform, value = waveform strength 0..100 (0 selects 10)
    // Relative changes, for event sources that should not have to track the
    // current strength themselves ("the character took a hit: +5").
    DglabNetCommand_IncreaseStrength = 4, ///< value = delta, channel 0 = both
    DglabNetCommand_DecreaseStrength = 5, ///< value = delta, channel 0 = both
} DglabNetCommand;

typedef struct {
    u32 port; ///< 0 selects DGLAB_NET_DEFAULT_PORT
} DglabNetStartRequest;

typedef struct {
    u32 command; ///< DglabNetCommand
    u32 channel; ///< 1 = A, 2 = B, 0 = both channels
    u32 value;
    u32 reserved; ///< unused, kept so the payload layout stays stable
} DglabNetSendRequest;

/// Enough for the longest QR payload, see docs/dglab-socket.md.
#define DGLAB_NET_QR_MAX 192u

/// Kept small so the reply still fits the inline IPC data area.
#define DGLAB_NET_LOG_CHUNK_SIZE 160u

typedef struct {
    u32 state;         ///< DglabNetState
    u32 last_result;   ///< Result of the last failing step, 0 when none
    u32 port;
    u32 ip;            ///< IPv4 address as struct in_addr, 0 while unknown
    u32 clients;       ///< Currently connected clients
    u32 paired;        ///< 1 while the App is bound
    u32 sessions;      ///< WebSocket connections accepted since the server started
    u32 heartbeats_sent;
    u32 messages_in;
    u32 messages_out;
    u32 commands_sent;    ///< Strength, clear and pulse commands sent to the App
    u32 reports_received; ///< Strength and feedback reports received from the App
    u32 last_error;       ///< Last protocol error code sent to a client, 0 when none
    u32 app_strength_a;   ///< Last strength report from the App
    u32 app_strength_b;
    u32 app_limit_a;
    u32 app_limit_b;
    u32 app_feedback; ///< Last feedback button, DGLAB_NET_FEEDBACK_NONE while none

    u8 ip_text[16];                     ///< Dotted quad, empty while the Switch has no address
    u8 controller_id[DGLAB_NET_ID_LEN]; ///< Controller id the QR code carries
    u8 peer_id[DGLAB_NET_ID_LEN];       ///< App id, empty while unbound
} DglabNetStatus;

typedef struct {
    u32 cursor;
} DglabNetLogRequest;

typedef struct {
    u32 next_cursor;
    u32 size;
    char text[DGLAB_NET_LOG_CHUNK_SIZE];
} DglabNetLogChunk;

typedef struct {
    u32 size;
    char text[DGLAB_NET_QR_MAX];
} DglabNetQrChunk;

// ---------------------------------------------------------------------------
// Waveform upload (event sources)
//
// The App plays a pulse segment once and then stops, so anything continuous has
// to be fed. Slots are the unit the protocol uses: one slot is 25ms of output
// and carries the waveform's frequency and its strength; the channel strength
// stays whatever the user set.
//
// A slot is exactly the shape the protocol layer already works with
// (DglabCoyoteV3WaveformEntry), which is what makes the packing reusable.
// ---------------------------------------------------------------------------

/// Slots one upload can carry. 48 slots are 1.2s of output and keep the request
/// inside the inline IPC payload (208 of 232 usable bytes).
#define DGLAB_NET_WAVEFORM_MAX_SLOTS 48u

/// Output one slot covers. The server paces its feeding by this, so a producer
/// has to emit slots at this rate to keep the stream continuous.
#define DGLAB_NET_WAVEFORM_SLOT_MS 25u

typedef struct {
    u16 frequency_ms; ///< 10..1000, the app side value; compressed before sending
    u8 strength;      ///< 0..100 waveform strength, multiplied by the channel strength
    u8 pad;
} DglabNetWaveformSlot;

typedef enum {
    /// Queue these slots behind whatever is already playing: the way a sensor
    /// stream keeps a waveform going.
    DglabNetWaveform_Append = 0,
    /// Drop what is playing and start these slots now: the way an event ("the
    /// character was hit") replaces the current gesture.
    DglabNetWaveform_Replace = 1,
} DglabNetWaveformMode;

typedef struct {
    u32 channel; ///< 1 = A, 2 = B, 0 = both channels
    u32 mode;    ///< DglabNetWaveformMode
    u32 slot_count;
    u32 pad;
    DglabNetWaveformSlot slots[DGLAB_NET_WAVEFORM_MAX_SLOTS];
} DglabNetWaveformRequest;

// ---------------------------------------------------------------------------
// BLE mode
// ---------------------------------------------------------------------------

typedef enum {
    DglabBleState_Idle = 0,       ///< No session.
    DglabBleState_Connecting,     ///< Scanning/linking, see docs/ble-poc.md.
    DglabBleState_Connected,      ///< Linked, subscribed and streaming.
    DglabBleState_Failed,         ///< Stopped by an error, see last_result.
} DglabBleState;

typedef struct {
    // The ceiling the device itself enforces (BF soft limit, 0..200). Start
    // with a small value: anything the client asks for later is clamped to it,
    // and 0 means the device cannot output at all. The probe's reaction test
    // uses 20; the value is the caller's call, this is only the contract.
    u32 soft_limit;
    // The device to drive. The console discovers this itself (see
    // docs/ble-poc.md, "设备地址：自动发现") and hands it over; all zeroes is
    // rejected rather than guessed.
    u8 address[6];
    u8 pad[2];
} DglabBleStartRequest;

typedef struct {
    u32 state;      ///< DglabBleState
    u32 packets;    ///< B0 packets written this session.
    u32 last_result; ///< Result of the last failing step, 0 when none.
    u8 address[6];  ///< The device the session is talking to.
    u8 connected;   ///< 1 once linked and subscribed.
    // Open loop: the device never reports back (docs/ble-re.md, "连接所有权在
    // 服务层是封的"), so these are the strengths this side last asked for, not
    // what the device is actually doing.
    u8 strength_a;
    u8 strength_b;
    u8 pad[3];
} DglabBleStatus;

typedef struct {
    /// The new ceiling (BF soft limit, 0..200). Out of range is rejected rather
    /// than clamped: a client that asks for 0xFFFFFFFF wants to know.
    u32 soft_limit;
} DglabBleLimitRequest;
