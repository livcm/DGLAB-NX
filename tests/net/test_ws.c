// Host side tests for the server side WebSocket implementation.
//
// Run with: make -C tests/net

#include <dglab/net/ws.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_checks;
static int g_failures;

#define CHECK(condition)                                                \
    do {                                                                \
        g_checks++;                                                     \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

// ---------------------------------------------------------------------------
// In-memory transport: feeds bytes from a script and records what is written.
// ---------------------------------------------------------------------------

typedef struct {
    const uint8_t* rx_data;
    size_t rx_size;
    size_t rx_offset;
    size_t rx_chunk; // bytes handed out per read, to exercise partial reads
    uint8_t tx[4096];
    size_t tx_size;
} MockIo;

static int mockRead(void* context, uint8_t* buffer, size_t size)
{
    MockIo* io = context;
    size_t available = io->rx_size - io->rx_offset;

    if (available == 0)
        return 0; // EOF

    size_t step = io->rx_chunk ? io->rx_chunk : available;

    if (step > size)
        step = size;

    if (step > available)
        step = available;

    memcpy(buffer, io->rx_data + io->rx_offset, step);
    io->rx_offset += step;

    return (int)step;
}

static int mockWrite(void* context, const uint8_t* buffer, size_t size)
{
    MockIo* io = context;

    if (io->tx_size + size > sizeof(io->tx))
        return -1;

    memcpy(io->tx + io->tx_size, buffer, size);
    io->tx_size += size;

    return (int)size;
}

static void ioInit(WsConn* conn, MockIo* io, const uint8_t* data, size_t size, size_t chunk)
{
    memset(io, 0, sizeof(*io));
    io->rx_data = data;
    io->rx_size = size;
    io->rx_chunk = chunk;

    memset(conn, 0, sizeof(*conn));
    conn->read = mockRead;
    conn->write = mockWrite;
    conn->context = io;
}

static void expectBytes(const char* name, const uint8_t* actual, const uint8_t* expected,
    size_t size)
{
    g_checks++;

    if (memcmp(actual, expected, size) != 0) {
        printf("FAIL %s\n  actual   =", name);
        for (size_t i = 0; i < size; i++)
            printf(" %02X", actual[i]);
        printf("\n  expected =");
        for (size_t i = 0; i < size; i++)
            printf(" %02X", expected[i]);
        printf("\n");
        g_failures++;
    }
}

static void expectHexBytes(const char* name, const uint8_t* actual, const char* expected_hex,
    size_t size)
{
    uint8_t expected[128];

    g_checks++;

    if (strlen(expected_hex) != size * 2) {
        printf("FAIL %s: bad test vector\n", name);
        g_failures++;
        return;
    }

    for (size_t i = 0; i < size; i++) {
        unsigned int value = 0;

        if (sscanf(expected_hex + i * 2, "%2x", &value) != 1) {
            printf("FAIL %s: bad test vector\n", name);
            g_failures++;
            return;
        }

        expected[i] = (uint8_t)value;
    }

    expectBytes(name, actual, expected, size);
}

// ---------------------------------------------------------------------------
// SHA-1 and base64
// ---------------------------------------------------------------------------

static void testSha1(void)
{
    static const struct {
        const char* input;
        const char* expected;
    } kCases[] = {
        { "", "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
        { "abc", "a9993e364706816aba3e25717850c26c9cd0d89d" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
    };

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        uint8_t digest[20];

        wsSha1((const uint8_t*)kCases[i].input, strlen(kCases[i].input), digest);
        expectHexBytes("sha1", digest, kCases[i].expected, sizeof(digest));
    }
}

static void testBase64(void)
{
    static const struct {
        const char* input;
        const char* expected;
    } kCases[] = {
        { "", "" },   { "f", "Zg==" },  { "fo", "Zm8=" },    { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char encoded[32];
        size_t length = wsBase64Encode((const uint8_t*)kCases[i].input, strlen(kCases[i].input),
            encoded, sizeof(encoded));

        g_checks++;
        if (length != strlen(kCases[i].expected) || strcmp(encoded, kCases[i].expected) != 0) {
            printf("FAIL base64(\"%s\") = \"%s\", expected \"%s\"\n", kCases[i].input, encoded,
                kCases[i].expected);
            g_failures++;
        }
    }
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

static const char* kRequest = "GET /1a2b3c HTTP/1.1\r\n"
                              "Host: 192.168.1.10:9999\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\n"
                              "\r\n";

static void testHandshake(void)
{
    WsHandshake handshake;
    char response[256];
    size_t length;

    CHECK(wsParseHandshake((const uint8_t*)kRequest, strlen(kRequest), &handshake));
    CHECK(strcmp(handshake.target, "/1a2b3c") == 0);
    CHECK(strcmp(handshake.key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);

    length = wsBuildHandshakeResponse(&handshake, response, sizeof(response));
    CHECK(length > 0);
    CHECK(strstr(response, "HTTP/1.1 101 Switching Protocols\r\n") == response);
    CHECK(strstr(response, "Upgrade: websocket\r\n") != NULL);
    CHECK(strstr(response, "Connection: Upgrade\r\n") != NULL);
    // RFC 6455 section 1.3 known answer.
    CHECK(strstr(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != NULL);

    const char* broken = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(!wsParseHandshake((const uint8_t*)broken, strlen(broken), &handshake));
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// RFC 6455 section 5.7: a masked "Hello" text frame.
static const uint8_t kMaskedHello[] = { 0x81, 0x85, 0x37, 0xFA, 0x21, 0x3D,
                                        0x7F, 0x9F, 0x4D, 0x51, 0x58 };

static void testFrameDecode(void)
{
    MockIo io;
    WsConn conn;
    uint8_t payload[64];
    size_t size = 0;
    WsOpcode opcode = 0;

    ioInit(&conn, &io, kMaskedHello, sizeof(kMaskedHello), 1); // byte at a time

    CHECK(wsConnRecv(&conn, &opcode, payload, sizeof(payload), &size));
    CHECK(opcode == WsOpcode_Text);
    CHECK(size == 5);
    CHECK(memcmp(payload, "Hello", 5) == 0);
    CHECK(io.tx_size == 0); // nothing had to be answered
}

static void testServerFrameEncode(void)
{
    MockIo io;
    WsConn conn;

    ioInit(&conn, &io, NULL, 0, 0);

    CHECK(wsConnSendText(&conn, "hi", 2));

    const uint8_t expected[4] = { 0x81, 0x02, 'h', 'i' };
    CHECK(io.tx_size == sizeof(expected));
    expectBytes("server text frame", io.tx, expected, sizeof(expected));
}

static void testPingIsAnswered(void)
{
    // Masked ping with payload "hi", followed by the masked "Hello" text frame.
    static const uint8_t kPing[] = { 0x89, 0x82, 0x01, 0x02, 0x03, 0x04, 'h' ^ 0x01,
                                     'i' ^ 0x02 };
    uint8_t script[sizeof(kPing) + sizeof(kMaskedHello)];
    MockIo io;
    WsConn conn;
    uint8_t payload[64];
    size_t size = 0;
    WsOpcode opcode = 0;

    memcpy(script, kPing, sizeof(kPing));
    memcpy(script + sizeof(kPing), kMaskedHello, sizeof(kMaskedHello));

    ioInit(&conn, &io, script, sizeof(script), 1);

    CHECK(wsConnRecv(&conn, &opcode, payload, sizeof(payload), &size));
    CHECK(opcode == WsOpcode_Text);
    CHECK(size == 5 && memcmp(payload, "Hello", 5) == 0);

    // The pong must have been written before the data frame was returned.
    const uint8_t expected[4] = { 0x8A, 0x02, 'h', 'i' };
    CHECK(io.tx_size == sizeof(expected));
    expectBytes("pong", io.tx, expected, sizeof(expected));
}

static void testCloseEndsConnection(void)
{
    // Masked close frame with code 1000.
    static const uint8_t kClose[] = { 0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8 };
    MockIo io;
    WsConn conn;
    uint8_t payload[64];
    size_t size = 0;
    WsOpcode opcode = 0;

    ioInit(&conn, &io, kClose, sizeof(kClose), 0);

    CHECK(!wsConnRecv(&conn, &opcode, payload, sizeof(payload), &size));

    // The close is echoed back: header plus the two payload bytes from the frame.
    const uint8_t expected[4] = { 0x88, 0x02, 0x03, 0xE8 };
    CHECK(io.tx_size == sizeof(expected));
    expectBytes("close reply", io.tx, expected, sizeof(expected));
}

static void testProtocolErrors(void)
{
    MockIo io;
    WsConn conn;
    uint8_t payload[64];
    size_t size = 0;
    WsOpcode opcode = 0;

    // Unmasked client frame: RFC 6455 requires masking.
    static const uint8_t kUnmasked[] = { 0x81, 0x02, 'h', 'i' };
    ioInit(&conn, &io, kUnmasked, sizeof(kUnmasked), 0);
    CHECK(!wsConnRecv(&conn, &opcode, payload, sizeof(payload), &size));

    // Payload larger than the DG-LAB limit.
    static const uint8_t kTooLarge[] = { 0x81, 0xFE, 0x13, 0x88, 0, 0, 0, 0 };
    ioInit(&conn, &io, kTooLarge, sizeof(kTooLarge), 0);
    CHECK(!wsConnRecv(&conn, &opcode, payload, sizeof(payload), &size));

    // Continuation frame without a started message.
    static const uint8_t kStrayContinuation[] = { 0x80, 0x80, 0, 0, 0, 0 };
    ioInit(&conn, &io, kStrayContinuation, sizeof(kStrayContinuation), 0);
    CHECK(!wsConnRecv(&conn, &opcode, payload, sizeof(payload), &size));
}

// ---------------------------------------------------------------------------
// Loopback test with real sockets
// ---------------------------------------------------------------------------

typedef struct {
    int fd;
} SocketIo;

static int socketRead(void* context, uint8_t* buffer, size_t size)
{
    SocketIo* io = context;

    return (int)recv(io->fd, buffer, size, 0);
}

static int socketWrite(void* context, const uint8_t* buffer, size_t size)
{
    SocketIo* io = context;

    return (int)send(io->fd, buffer, size, 0);
}

static void testLoopback(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;

    // Listening sockets are not always available (for example inside a sandbox),
    // so this test reports a skip instead of a failure in that case.
    if (listener < 0) {
        printf("SKIP loopback: socket() unavailable\n");
        return;
    }

    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        printf("SKIP loopback: cannot listen on loopback\n");
        close(listener);
        return;
    }

    socklen_t address_len = sizeof(address);
    CHECK(getsockname(listener, (struct sockaddr*)&address, &address_len) == 0);

    int client = socket(AF_INET, SOCK_STREAM, 0);

    if (client < 0 || connect(client, (struct sockaddr*)&address, sizeof(address)) != 0) {
        printf("SKIP loopback: cannot connect\n");
        if (client >= 0)
            close(client);
        close(listener);
        return;
    }

    int server = accept(listener, NULL, NULL);
    CHECK(server >= 0);

    SocketIo server_io = { .fd = server };
    WsConn conn = { .read = socketRead, .write = socketWrite, .context = &server_io };

    // The client sends the handshake and pipelines the first frame.
    CHECK(send(client, kRequest, strlen(kRequest), 0) == (ssize_t)strlen(kRequest));
    CHECK(send(client, kMaskedHello, sizeof(kMaskedHello), 0) == (ssize_t)sizeof(kMaskedHello));

    CHECK(wsConnHandshake(&conn));

    char response[256];
    ssize_t got = recv(client, response, sizeof(response) - 1, 0);
    CHECK(got > 0);

    if (got > 0) {
        response[got] = '\0';
        CHECK(strstr(response, "101 Switching Protocols") != NULL);
        CHECK(strstr(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    }

    uint8_t payload[64];
    size_t size = 0;
    WsOpcode opcode = 0;

    // The frame pipelined behind the handshake must still be readable.
    CHECK(wsConnRecv(&conn, &opcode, payload, sizeof(payload), &size));
    CHECK(opcode == WsOpcode_Text);
    CHECK(size == 5 && memcmp(payload, "Hello", 5) == 0);

    CHECK(wsConnSendText(&conn, "hi", 2));

    uint8_t raw[16];
    size_t raw_len = 0;

    // The server writes the header and the payload with separate send() calls,
    // so the client has to loop like a real WebSocket client would.
    while (raw_len < 4) {
        got = recv(client, raw + raw_len, sizeof(raw) - raw_len, 0);

        if (got <= 0)
            break;

        raw_len += (size_t)got;
    }

    const uint8_t expected[4] = { 0x81, 0x02, 'h', 'i' };
    CHECK(raw_len == sizeof(expected));

    if (raw_len == sizeof(expected))
        expectBytes("loopback server frame", raw, expected, sizeof(expected));

    close(client);
    close(server);
    close(listener);
}

int main(void)
{
    testSha1();
    testBase64();
    testHandshake();
    testFrameDecode();
    testServerFrameEncode();
    testPingIsAnswered();
    testCloseEndsConnection();
    testProtocolErrors();
    testLoopback();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
