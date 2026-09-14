#pragma once

// Minimal server side WebSocket (RFC 6455).
//
// The DG-LAB App connects to us as a WebSocket client, so the Switch side only
// needs the server half: parse the HTTP upgrade request, answer with
// Sec-WebSocket-Accept, then read masked client frames and write unmasked server
// frames.
//
// The implementation is platform independent: the caller supplies read/write
// callbacks, so the same code runs on the Switch (libnx BSD sockets) and in the
// host tests (POSIX sockets).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// DG-LAB limits a socket message to 1950 bytes; keep a little headroom.
#define WS_MAX_MESSAGE 2048u
#define WS_MAX_HANDSHAKE 1536u

typedef enum {
    WsOpcode_Continuation = 0x0,
    WsOpcode_Text = 0x1,
    WsOpcode_Binary = 0x2,
    WsOpcode_Close = 0x8,
    WsOpcode_Ping = 0x9,
    WsOpcode_Pong = 0xA,
} WsOpcode;

// Parsed HTTP upgrade request.
typedef struct {
    char target[192]; // request target, e.g. "/<clientId>" or "/?tid=<clientId>"
    char key[48];     // Sec-WebSocket-Key, base64
    char protocol[64]; // Sec-WebSocket-Protocol, empty when the client sent none
} WsHandshake;

// Returns > 0 for the byte count read, 0 on EOF, < 0 on error.
typedef int (*WsReadFn)(void* context, uint8_t* buffer, size_t size);
// Returns the byte count written, or < 0 on error.
typedef int (*WsWriteFn)(void* context, const uint8_t* buffer, size_t size);

typedef struct {
    WsReadFn read;
    WsWriteFn write;
    void* context;
    uint8_t rx[WS_MAX_MESSAGE + 64];
    size_t rx_len;
    bool handshake_done;
    // Request target of the handshake, for example "/<clientId>" or "/?tid=<clientId>".
    // The DG-LAB Socket protocol carries the controller id here, so the layer
    // above needs it after the handshake to decide how to pair the connection.
    char target[192];
} WsConn;

// SHA-1 (RFC 3174) and base64 (RFC 4648) helpers, exposed for the host tests.
void wsSha1(const uint8_t* data, size_t size, uint8_t out[20]);
size_t wsBase64Encode(const uint8_t* data, size_t size, char* out, size_t out_size);

// Parses a complete HTTP upgrade request. Returns false when the request is
// incomplete or lacks the required headers.
bool wsParseHandshake(const uint8_t* request, size_t size, WsHandshake* out);

// Builds the "101 Switching Protocols" response. Returns its length, or 0 when
// it does not fit.
size_t wsBuildHandshakeResponse(const WsHandshake* handshake, char* out, size_t out_size);

// Reads the request and writes the response. The connection is then ready.
bool wsConnHandshake(WsConn* conn);

// Receives one data frame (text or binary). Ping is answered automatically,
// pong is ignored, close ends the connection and is answered with a close frame.
// Returns false on close, protocol error or I/O failure.
bool wsConnRecv(WsConn* conn, WsOpcode* opcode, uint8_t* payload, size_t payload_size,
    size_t* out_size);

// Sends one frame (unmasked, as a server must).
bool wsConnSend(WsConn* conn, WsOpcode opcode, const uint8_t* payload, size_t size);

static inline bool wsConnSendText(WsConn* conn, const char* text, size_t size)
{
    return wsConnSend(conn, WsOpcode_Text, (const uint8_t*)text, size);
}
