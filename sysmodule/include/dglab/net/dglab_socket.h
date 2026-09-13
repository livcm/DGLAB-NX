#pragma once

// DG-LAB App "Socket" protocol, V3 variant.
//
// The App is the WebSocket client; we are the server. Message format and command
// strings follow docs/dglab-socket.md, which is based on the official
// dglab-websocket-server implementation and the PyDGLab-WS reference.
//
// Everything here is platform independent so it can be tested on the host.

#include <dglab/protocol/coyote_v3.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DGLAB_SOCKET_MAX_MESSAGE 1950u
#define DGLAB_SOCKET_ID_LEN 40u

typedef enum {
    DglabSocketType_Unknown = 0,
    DglabSocketType_Heartbeat,
    DglabSocketType_Bind,
    DglabSocketType_Msg,
    DglabSocketType_Break,
    DglabSocketType_Error,
} DglabSocketType;

// V3 uses numbers in strength/clear commands and letters in pulse commands.
typedef enum {
    DglabSocketChannel_A = 1,
    DglabSocketChannel_B = 2,
} DglabSocketChannel;

// Matches StrengthOperationType in the reference implementation.
typedef enum {
    DglabSocketStrength_Decrease = 0,
    DglabSocketStrength_Increase = 1,
    DglabSocketStrength_SetTo = 2,
} DglabSocketStrengthOp;

typedef struct {
    DglabSocketType type;
    char client_id[DGLAB_SOCKET_ID_LEN];
    char target_id[DGLAB_SOCKET_ID_LEN];
    char message[DGLAB_SOCKET_MAX_MESSAGE];
    int code; // numeric value of `message` when it is all digits, otherwise -1
} DglabSocketMessage;

// Parses one JSON envelope. Only the top level keys type/clientId/targetId/
// message are read, which is all the protocol uses.
bool dglabSocketParseMessage(const char* text, size_t size, DglabSocketMessage* out);

// Builds one JSON envelope. Returns its length, or 0 when it does not fit.
size_t dglabSocketBuildMessage(char* out, size_t out_size, const char* type, const char* client_id,
    const char* target_id, const char* message);

const char* dglabSocketTypeName(DglabSocketType type);

// Commands sent to the App.
size_t dglabSocketBuildStrength(char* out, size_t out_size, DglabSocketChannel channel,
    DglabSocketStrengthOp operation, int value);
size_t dglabSocketBuildClear(char* out, size_t out_size, DglabSocketChannel channel);

// pulse-<A|B>:["<16 hex>", ...]
size_t dglabSocketBuildPulse(char* out, size_t out_size, DglabSocketChannel channel,
    const char* const* pulse_hex, size_t count);

// Reports coming from the App.
typedef struct {
    int a;
    int b;
    int a_limit;
    int b_limit;
} DglabSocketStrengthData;

bool dglabSocketParseStrengthReport(const char* message, DglabSocketStrengthData* out);
bool dglabSocketParseFeedback(const char* message, int* out_button);

// Encodes four BLE V3 waveform slots (4 frequencies then 4 strengths) as the
// 16 hex characters the pulse command expects. out must hold 17 bytes.
void dglabSocketEncodePulseHex(const DglabCoyoteV3WaveformSlot* slots, char* out);

// https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#<uri>/<client_id>
size_t dglabSocketBuildQrUrl(char* out, size_t out_size, const char* ws_uri,
    const char* client_id);
