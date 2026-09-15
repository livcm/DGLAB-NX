#include <dglab/net/ws.h>

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// SHA-1
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t state[5];
    uint64_t bit_count;
    uint8_t block[64];
    size_t block_len;
} Sha1;

static uint32_t rotl32(uint32_t value, unsigned bits)
{
    return (value << bits) | (value >> (32u - bits));
}

static void sha1Block(Sha1* sha, const uint8_t block[64])
{
    uint32_t w[80];

    for (unsigned i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }

    for (unsigned i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = sha->state[0];
    uint32_t b = sha->state[1];
    uint32_t c = sha->state[2];
    uint32_t d = sha->state[3];
    uint32_t e = sha->state[4];

    for (unsigned i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        uint32_t temp = rotl32(a, 5) + f + e + k + w[i];

        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
}

void wsSha1(const uint8_t* data, size_t size, uint8_t out[20])
{
    Sha1 sha;

    sha.state[0] = 0x67452301u;
    sha.state[1] = 0xEFCDAB89u;
    sha.state[2] = 0x98BADCFEu;
    sha.state[3] = 0x10325476u;
    sha.state[4] = 0xC3D2E1F0u;
    sha.bit_count = 0;
    sha.block_len = 0;

    for (size_t i = 0; i < size; i++) {
        sha.block[sha.block_len++] = data[i];

        if (sha.block_len == 64) {
            sha1Block(&sha, sha.block);
            sha.bit_count += 512;
            sha.block_len = 0;
        }
    }

    sha.bit_count += (uint64_t)sha.block_len * 8u;
    sha.block[sha.block_len++] = 0x80;

    if (sha.block_len > 56) {
        while (sha.block_len < 64)
            sha.block[sha.block_len++] = 0;

        sha1Block(&sha, sha.block);
        sha.block_len = 0;
    }

    while (sha.block_len < 56)
        sha.block[sha.block_len++] = 0;

    for (unsigned i = 0; i < 8; i++)
        sha.block[56 + i] = (uint8_t)(sha.bit_count >> (56 - 8 * i));

    sha1Block(&sha, sha.block);

    for (unsigned i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(sha.state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(sha.state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(sha.state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)sha.state[i];
    }
}

// ---------------------------------------------------------------------------
// base64
// ---------------------------------------------------------------------------

size_t wsBase64Encode(const uint8_t* data, size_t size, char* out, size_t out_size)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((size + 2) / 3) * 4;

    if (out_size < needed + 1)
        return 0;

    size_t written = 0;

    for (size_t i = 0; i < size; i += 3) {
        uint32_t value = (uint32_t)data[i] << 16;
        size_t remaining = size - i;

        if (remaining > 1)
            value |= (uint32_t)data[i + 1] << 8;

        if (remaining > 2)
            value |= (uint32_t)data[i + 2];

        out[written++] = table[(value >> 18) & 0x3F];
        out[written++] = table[(value >> 12) & 0x3F];
        out[written++] = (remaining > 1) ? table[(value >> 6) & 0x3F] : '=';
        out[written++] = (remaining > 2) ? table[value & 0x3F] : '=';
    }

    out[written] = '\0';
    return written;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static bool isHeaderName(const char* line, size_t line_len, const char* name)
{
    size_t name_len = strlen(name);

    if (line_len < name_len + 1)
        return false;

    for (size_t i = 0; i < name_len; i++) {
        char c = line[i];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');

        if (c != name[i])
            return false;
    }

    return line[name_len] == ':';
}

static void copyHeaderValue(const char* value, size_t value_len, char* out, size_t out_size)
{
    while (value_len && (*value == ' ' || *value == '\t')) {
        value++;
        value_len--;
    }

    // The value runs up to the CR of the header's CRLF.
    while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t' ||
                            value[value_len - 1] == '\r' || value[value_len - 1] == '\n'))
        value_len--;

    if (value_len > out_size - 1)
        value_len = out_size - 1;

    memcpy(out, value, value_len);
    out[value_len] = '\0';
}

bool wsParseHandshake(const uint8_t* request, size_t size, WsHandshake* out)
{
    size_t target_len = 0;
    size_t i;

    if (size < 16 || size > WS_MAX_HANDSHAKE)
        return false;

    memset(out, 0, sizeof(*out));

    // Request line: "GET <target> HTTP/1.1".
    if (memcmp(request, "GET ", 4) != 0)
        return false;

    i = 4;

    while (i < size && request[i] != ' ' && target_len < sizeof(out->target) - 1)
        out->target[target_len++] = (char)request[i++];

    out->target[target_len] = '\0';

    if (target_len == 0)
        return false;

    // Walk the request line and then every header line. The first empty line
    // ends the header block.
    size_t cursor = 0;

    while (cursor < size) {
        size_t line_end = cursor;

        while (line_end + 1 < size && !(request[line_end] == '\r' && request[line_end + 1] == '\n'))
            line_end++;

        if (line_end + 1 >= size || line_end == cursor)
            break;

        size_t line_len = line_end - cursor;

        // "Sec-WebSocket-Key" is 17 characters, so the value starts after the
        // colon, at offset 18.
        if (line_len > 18 &&
            isHeaderName((const char*)request + cursor, line_len, "sec-websocket-key"))
            copyHeaderValue((const char*)request + cursor + 18, line_len - 18, out->key,
                sizeof(out->key));

        // "Sec-WebSocket-Protocol" is 22 characters.
        if (line_len > 23 &&
            isHeaderName((const char*)request + cursor, line_len, "sec-websocket-protocol"))
            copyHeaderValue((const char*)request + cursor + 23, line_len - 23, out->protocol,
                sizeof(out->protocol));

        cursor = line_end + 2;
    }

    return out->key[0] != '\0';
}

size_t wsBuildHandshakeResponse(const WsHandshake* handshake, char* out, size_t out_size)
{
    uint8_t digest[20];
    char accept[32];
    char source[128];
    char protocol[96];
    int source_len;
    int written;

    source_len = snprintf(source, sizeof(source), "%s%s", handshake->key, WS_GUID);

    if (source_len <= 0 || (size_t)source_len >= sizeof(source))
        return 0;

    wsSha1((const uint8_t*)source, (size_t)source_len, digest);

    if (wsBase64Encode(digest, sizeof(digest), accept, sizeof(accept)) == 0)
        return 0;

    // A client that offers subprotocols requires the server to pick one, so the
    // first offered token is echoed back. The App is not known to need this, but
    // a client that does would otherwise sit in "connecting" forever.
    protocol[0] = '\0';

    if (handshake->protocol[0] != '\0') {
        size_t len = 0;

        // Only the first token of the comma separated list.
        while (handshake->protocol[len] != '\0' && handshake->protocol[len] != ',' &&
               handshake->protocol[len] != ' ' && len < sizeof(protocol) - 24)
            len++;

        if (len != 0)
            snprintf(protocol, sizeof(protocol), "Sec-WebSocket-Protocol: %.*s\r\n", (int)len,
                handshake->protocol);
    }

    written = snprintf(out, out_size,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "%s"
        "\r\n",
        accept, protocol);

    if (written <= 0 || (size_t)written >= out_size)
        return 0;

    return (size_t)written;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

bool wsConnSend(WsConn* conn, WsOpcode opcode, const uint8_t* payload, size_t size)
{
    uint8_t header[10];
    size_t header_len = 0;

    header[0] = (uint8_t)(0x80u | (uint8_t)opcode);

    if (size < 126) {
        header[1] = (uint8_t)size;
        header_len = 2;
    } else if (size <= 0xFFFF) {
        header[1] = 126;
        header[2] = (uint8_t)(size >> 8);
        header[3] = (uint8_t)size;
        header_len = 4;
    } else {
        header[1] = 127;
        for (unsigned i = 0; i < 8; i++)
            header[2 + i] = (uint8_t)((uint64_t)size >> (56 - 8 * i));
        header_len = 10;
    }

    if (conn->write(conn->context, header, header_len) != (int)header_len)
        return false;

    if (size && conn->write(conn->context, payload, size) != (int)size)
        return false;

    return true;
}

// Reads exactly `size` bytes into the connection buffer.
static bool wsReadExact(WsConn* conn, size_t size)
{
    while (conn->rx_len < size) {
        int read = conn->read(conn->context, conn->rx + conn->rx_len, size - conn->rx_len);

        if (read <= 0)
            return false;

        conn->rx_len += (size_t)read;
    }

    return true;
}

// Reads one raw frame, unmasking it in place inside conn->rx.
static bool wsReadFrame(WsConn* conn, WsOpcode* out_opcode, bool* out_fin, size_t* out_header,
    size_t* out_size)
{
    uint8_t mask[4];
    size_t payload_len;
    size_t header_len;
    bool masked;

    if (!wsReadExact(conn, 2))
        return false;

    bool fin = (conn->rx[0] & 0x80u) != 0;
    uint8_t opcode = conn->rx[0] & 0x0Fu;
    masked = (conn->rx[1] & 0x80u) != 0;
    payload_len = conn->rx[1] & 0x7Fu;

    if (payload_len == 126) {
        if (!wsReadExact(conn, 4))
            return false;

        payload_len = ((size_t)conn->rx[2] << 8) | conn->rx[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (!wsReadExact(conn, 10))
            return false;

        payload_len = 0;

        for (unsigned i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | conn->rx[2 + i];

        header_len = 10;
    } else {
        header_len = 2;
    }

    // A client must mask its frames (RFC 6455 section 5.1).
    if (!masked)
        return false;

    if (payload_len > WS_MAX_MESSAGE)
        return false;

    if (!wsReadExact(conn, header_len + 4 + payload_len))
        return false;

    memcpy(mask, conn->rx + header_len, 4);

    for (size_t i = 0; i < payload_len; i++)
        conn->rx[header_len + 4 + i] ^= mask[i % 4];

    *out_opcode = (WsOpcode)opcode;
    *out_fin = fin;
    *out_header = header_len + 4;
    *out_size = payload_len;

    return true;
}

static void wsDropFrame(WsConn* conn, size_t header_len, size_t payload_len)
{
    size_t consumed = header_len + payload_len;

    if (consumed < conn->rx_len)
        memmove(conn->rx, conn->rx + consumed, conn->rx_len - consumed);

    conn->rx_len -= consumed;
}

bool wsConnRecv(WsConn* conn, WsOpcode* opcode, uint8_t* payload, size_t payload_size,
    size_t* out_size)
{
    uint8_t message[WS_MAX_MESSAGE];
    size_t message_len = 0;
    bool started = false;
    WsOpcode data_opcode = WsOpcode_Text;

    for (;;) {
        WsOpcode frame_opcode;
        bool fin;
        size_t header_len;
        size_t frame_size;

        if (!wsReadFrame(conn, &frame_opcode, &fin, &header_len, &frame_size))
            return false;

        uint8_t* frame_payload = conn->rx + header_len;

        switch (frame_opcode) {
            case WsOpcode_Ping:
                conn->ping_count++;

                if (!wsConnSend(conn, WsOpcode_Pong, frame_payload, frame_size)) {
                    wsDropFrame(conn, header_len, frame_size);
                    return false;
                }
                wsDropFrame(conn, header_len, frame_size);
                continue;

            case WsOpcode_Pong:
                conn->pong_count++;
                wsDropFrame(conn, header_len, frame_size);
                continue;

            case WsOpcode_Close:
                conn->close_count++;
                (void)wsConnSend(conn, WsOpcode_Close, frame_payload, frame_size);
                wsDropFrame(conn, header_len, frame_size);
                return false;

            case WsOpcode_Text:
            case WsOpcode_Binary:
                if (started) {
                    wsDropFrame(conn, header_len, frame_size);
                    return false; // interleaved data frames are a protocol error
                }

                started = true;
                data_opcode = frame_opcode;
                break;

            case WsOpcode_Continuation:
                if (!started) {
                    wsDropFrame(conn, header_len, frame_size);
                    return false;
                }
                break;

            default:
                wsDropFrame(conn, header_len, frame_size);
                return false;
        }

        if (message_len + frame_size > sizeof(message)) {
            wsDropFrame(conn, header_len, frame_size);
            return false;
        }

        memcpy(message + message_len, frame_payload, frame_size);
        message_len += frame_size;
        wsDropFrame(conn, header_len, frame_size);

        if (!fin)
            continue;

        if (message_len > payload_size)
            return false;

        memcpy(payload, message, message_len);
        *opcode = data_opcode;
        *out_size = message_len;

        return true;
    }
}

bool wsConnHandshake(WsConn* conn)
{
    char response[256];
    WsHandshake handshake;
    size_t response_len;
    size_t total = 0;
    size_t header_end = 0;

    // Read until the end of the HTTP header block.
    while (total < WS_MAX_HANDSHAKE) {
        int read = conn->read(conn->context, conn->rx + total, WS_MAX_HANDSHAKE - total);

        if (read <= 0)
            return false;

        total += (size_t)read;

        if (total >= 4) {
            for (size_t i = 0; i + 4 <= total; i++) {
                if (memcmp(conn->rx + i, "\r\n\r\n", 4) == 0) {
                    header_end = i + 4;
                    goto complete;
                }
            }
        }
    }

    return false;

complete:
    if (!wsParseHandshake(conn->rx, header_end, &handshake))
        return false;

    response_len = wsBuildHandshakeResponse(&handshake, response, sizeof(response));

    if (response_len == 0)
        return false;

    if (conn->write(conn->context, (const uint8_t*)response, response_len) != (int)response_len)
        return false;

    // Keep any bytes that arrived after the header block: the client may already
    // have pipelined its first frame.
    size_t leftover = total - header_end;

    if (leftover)
        memmove(conn->rx, conn->rx + header_end, leftover);

    conn->rx_len = leftover;
    conn->handshake_done = true;
    memcpy(conn->target, handshake.target, sizeof(conn->target));

    return true;
}
