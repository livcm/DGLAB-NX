#pragma once

#include <switch/types.h>

// The service name is limited to 8 characters by the Switch service manager.
#define DGLAB_IPC_SERVICE_NAME "dglab"

// Minimal IPC protocol version. Increment whenever the IPC surface changes in a
// way that is not backward compatible.
#define DGLAB_IPC_PROTOCOL_VERSION 1u

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
} DglabNetCommand;

typedef struct {
    u32 port; ///< 0 selects DGLAB_NET_DEFAULT_PORT
} DglabNetStartRequest;

typedef struct {
    u32 command; ///< DglabNetCommand
    u32 channel; ///< 1 = A, 2 = B, 0 = both channels
    u32 value;
    u32 pad;
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
