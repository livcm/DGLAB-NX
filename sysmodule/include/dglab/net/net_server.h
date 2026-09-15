#pragma once

// DG-LAB Socket V3 server core.
//
// The Switch is the WebSocket server the phone App connects to, and it is also
// the controller: the id the QR code carries is generated here, not by a
// separate controller connection. The App is the controlled side and connects
// with that id in the request target ("/<id>"), which is where the pairing
// starts; see docs/dglab-socket.md.
//
// This file is platform independent on purpose. Socket setup, threads and
// locking live in sysmodule/source/transport/net_socket.c, so the host tests can
// drive the whole session over in-memory WebSocket connections (tests/net).

#include <dglab/ipc.h>
#include <dglab/net/ws.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Slots for client connections. Two slots let a reconnect happen while the old
// connection is still being torn down; only one client can be bound.
#define DGLAB_NET_MAX_CLIENTS 2u

// The reference server sends a heartbeat to bound clients every 30s.
#define DGLAB_NET_HEARTBEAT_INTERVAL_MS 30000u

// Ring buffer holding the log the NRO reads back through IPC.
#define DGLAB_NET_LOG_CAPACITY 4096u

// Messages fully written to the log. The App keeps the link alive with
// heartbeats, and logging every single one would flush everything else.
#define DGLAB_NET_LOG_MESSAGE_LIMIT 32u

// Pending waveform slots per channel. 128 slots are 3.2s, which is far more than
// an event source should ever fall behind by.
#define DGLAB_NET_WAVEFORM_QUEUE_SLOTS 128u

// Slots sent per pulse command: 32 slots are eight elements (800ms), well inside
// the 86 element limit and short enough for an event to be picked up quickly.
#define DGLAB_NET_WAVEFORM_BATCH_SLOTS 32u

// How much material stays queued on the App's side. The next batch goes out when
// the App is down to this much, so playback never runs dry.
#define DGLAB_NET_WAVEFORM_LEAD_MS 200u

// One slot covers this much output.
#define DGLAB_NET_WAVEFORM_SLOT_MS 25u

typedef struct {
    uint32_t port;
    // Milliseconds; only differences are used (heartbeat schedule, log stamps).
    uint64_t (*now_ms)(void* context);
    // Fills out with size random bytes. Used for uuid generation.
    bool (*fill_random)(void* context, uint8_t* out, size_t size);
    // Writes the Switch LAN address as both struct in_addr and a dotted quad;
    // the QR url needs the text. Returns false while the Switch has no address.
    bool (*get_ip)(void* context, uint32_t* address, char* text, size_t text_size);
    // Optional: receives every log line (no trailing newline). The sysmodule
    // uses it to write dglab-sys.log, which survives a crash the in-memory ring
    // does not. It must stay inert until the transport tells it to start.
    void (*log_sink)(void* context, const char* line);
    void* context;
} DglabNetServerConfig;

typedef struct {
    WsConn* conn;
    char id[DGLAB_NET_ID_LEN]; ///< uuid assigned to this client
    bool active;
    bool bound; ///< this client is the paired App
    uint64_t connected_at_ms;
    uint64_t last_message_ms;
    bool warned_silent; ///< the "App went quiet" line was already logged
    uint32_t messages_in;
} DglabNetClient;

typedef struct {
    DglabNetWaveformSlot slots[DGLAB_NET_WAVEFORM_QUEUE_SLOTS];
    size_t head; // next slot to send
    size_t count;
    uint64_t next_send_ms; // when the App is expected to run low
} DglabNetWaveformQueue;

typedef struct {
    DglabNetServerConfig config;
    DglabNetStatus status;
    char controller_id[DGLAB_NET_ID_LEN];
    DglabNetClient clients[DGLAB_NET_MAX_CLIENTS];
    DglabNetWaveformQueue waveform[2]; // A and B
    uint64_t last_heartbeat_ms;
    uint64_t waveform_logged_ms; ///< when the waveform stream last logged a batch
    uint32_t fallback_counter; ///< keeps generated ids distinct when there is no random source
    uint32_t messages_logged;
    char log[DGLAB_NET_LOG_CAPACITY];
    uint32_t log_write_offset;
    uint32_t log_valid_from;
} DglabNetServer;

typedef enum {
    DglabNetSend_Ok = 0,
    DglabNetSend_NotPaired,  ///< No App is bound.
    DglabNetSend_BadRequest, ///< Unknown command, channel or value.
    DglabNetSend_TooLong,    ///< The command does not fit a socket message.
    DglabNetSend_IoError,    ///< The transport refused to write.
} DglabNetSendResult;

// Generates the controller id and initialises the state machine. The server is
// idle until dglabNetServerSetListening() is called.
void dglabNetServerInit(DglabNetServer* server, const DglabNetServerConfig* config);

// Lifecycle transitions, called by the transport.
void dglabNetServerSetListening(DglabNetServer* server, uint16_t port);
void dglabNetServerSetStopped(DglabNetServer* server);
void dglabNetServerSetFailed(DglabNetServer* server, uint32_t result);

// Called by the transport for a connection whose WebSocket handshake completed.
// Returns false when the connection was rejected; the reason has already been
// reported to the client (as a protocol error frame) and to the log.
bool dglabNetServerAttach(DglabNetServer* server, WsConn* conn);

// Called by the transport when a connection is gone, for any reason.
void dglabNetServerDetach(DglabNetServer* server, WsConn* conn);

// Called by the transport for one complete WebSocket text message.
void dglabNetServerOnMessage(DglabNetServer* server, WsConn* conn, const char* text, size_t size);

// Called by the transport when the connection showed life that is not a
// protocol message, which for the App means a WebSocket ping.
void dglabNetServerOnActivity(DglabNetServer* server, WsConn* conn);

// Periodic work: heartbeats and stale connection detection. Call every 100ms.
void dglabNetServerPoll(DglabNetServer* server, uint64_t now_ms);

// Sends one command to the bound App.
DglabNetSendResult dglabNetServerSend(DglabNetServer* server, const DglabNetSendRequest* request);

// Queues (or replaces with) a batch of waveform slots for one or both channels.
// Anything queued is fed to the App as pulse commands; see the header of the
// request type for why that is needed.
DglabNetSendResult dglabNetServerUploadWaveform(DglabNetServer* server,
    const DglabNetWaveformRequest* request);

// Fills out with a snapshot of the status. Also refreshes the LAN address, so a
// reader that keeps polling sees the address appear once the Switch is online.
void dglabNetServerGetStatus(DglabNetServer* server, DglabNetStatus* out);

// Writes the QR payload. Returns false and an empty buffer while the Switch has
// no LAN address.
bool dglabNetServerGetQr(DglabNetServer* server, char* out, size_t out_size, size_t* out_written);

// Reads the log ring. Returns the cursor to pass in next time, so a reader that
// falls behind silently skips to the oldest line still in the ring.
uint32_t dglabNetServerReadLog(DglabNetServer* server, uint32_t cursor, char* out, size_t out_size);

// Appends one line to the log ring. The transport uses it for events that never
// reach the protocol layer (accept failures, disconnects).
void dglabNetServerLog(DglabNetServer* server, const char* fmt, ...);

// ---------------------------------------------------------------------------
// Exposed for the host tests.
// ---------------------------------------------------------------------------

// Formats 16 bytes as a lowercase uuid string. Returns its length, or 0 when it
// does not fit out_size.
size_t dglabNetFormatUuid(const uint8_t bytes[16], char* out, size_t out_size);

// Extracts the client id from a WebSocket request target. Both the V3 form
// ("/<id>") and the V4 form ("/?tid=<id>") are understood. Returns false when
// the target carries no id.
bool dglabNetParseTargetId(const char* target, char* out, size_t out_size);
