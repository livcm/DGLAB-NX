// End to end test for the socket server core over a real TCP connection.
//
// test_net_server.c drives the core with an in-memory connection. This test
// plays the phone App: it opens a TCP connection to a loopback listener, does
// the WebSocket handshake with the controller id in the request target, sends a
// masked strength report, and checks the frames the server writes back.
//
// A sandbox that forbids listening on loopback skips the test.
//
// Run with: make -C tests/net

#include <dglab/net/net_server.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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
// Harness
// ---------------------------------------------------------------------------

typedef struct {
    DglabNetServer server;
    uint64_t now;
    uint32_t random_calls;
} Harness;

static uint64_t harnessNow(void* context)
{
    return ((Harness*)context)->now;
}

static bool harnessRandom(void* context, uint8_t* out, size_t size)
{
    Harness* harness = context;

    harness->random_calls++;

    for (size_t i = 0; i < size; i++)
        out[i] = (uint8_t)(harness->random_calls * 16u + (uint32_t)i);

    return true;
}

static bool harnessIp(void* context, uint32_t* address, char* text, size_t text_size)
{
    (void)context;

    snprintf(text, text_size, "127.0.0.1");
    *address = 0x7F000001u;

    return true;
}

// ---------------------------------------------------------------------------
// Server side WebSocket connection over a TCP socket
// ---------------------------------------------------------------------------

static int socketRead(void* context, uint8_t* buffer, size_t size)
{
    int fd = *(int*)context;
    ssize_t got = recv(fd, buffer, size, 0);

    return (int)got;
}

static int socketWrite(void* context, const uint8_t* buffer, size_t size)
{
    int fd = *(int*)context;
    ssize_t sent = send(fd, buffer, size, 0);

    return (int)sent;
}

static void setTimeout(int fd)
{
    struct timeval timeout;

    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// Reads one server frame (never masked) and returns its text payload.
static bool readServerFrame(int fd, char* out, size_t out_size)
{
    uint8_t header[4];
    size_t header_len = 2;
    size_t len;

    if (recv(fd, header, 2, 0) != 2)
        return false;

    if ((header[0] & 0x0Fu) == 0x8u) // close
        return false;

    len = header[1] & 0x7Fu;

    if (len == 126) {
        if (recv(fd, header + 2, 2, 0) != 2)
            return false;

        len = ((size_t)header[2] << 8) | header[3];
        header_len = 4;
    }

    (void)header_len;

    if (len >= out_size)
        return false;

    if (len && recv(fd, out, len, 0) != (ssize_t)len)
        return false;

    out[len] = '\0';

    return true;
}

// Builds a masked client frame (a client must mask, RFC 6455 section 5.1).
static size_t buildClientFrame(uint8_t* out, size_t out_size, const char* text,
    const uint8_t mask[4])
{
    size_t len = strlen(text);

    if (len >= 126 || out_size < len + 6)
        return 0;

    out[0] = 0x81; // fin + text
    out[1] = (uint8_t)(0x80u | len);
    memcpy(out + 2, mask, 4);

    for (size_t i = 0; i < len; i++)
        out[6 + i] = (uint8_t)text[i] ^ mask[i % 4];

    return len + 6;
}

// Same, for a control frame such as a ping.
static size_t buildClientControlFrame(uint8_t* out, size_t out_size, uint8_t opcode,
    const uint8_t mask[4])
{
    if (out_size < 6)
        return 0;

    out[0] = (uint8_t)(0x80u | opcode);
    out[1] = 0x80; // masked, empty payload
    memcpy(out + 2, mask, 4);

    return 6;
}

static bool sendAll(int fd, const uint8_t* data, size_t size)
{
    size_t sent = 0;

    while (sent < size) {
        ssize_t rc = send(fd, data + sent, size - sent, 0);

        if (rc <= 0)
            return false;

        sent += (size_t)rc;
    }

    return true;
}

static bool readHandshakeResponse(int fd)
{
    char response[512];
    size_t total = 0;

    while (total < sizeof(response) - 1) {
        ssize_t got = recv(fd, response + total, sizeof(response) - 1 - total, 0);

        if (got <= 0)
            return false;

        total += (size_t)got;
        response[total] = '\0';

        if (strstr(response, "\r\n\r\n") != NULL)
            return strstr(response, "101 Switching Protocols") != NULL;
    }

    return false;
}

// ---------------------------------------------------------------------------
// The test
// ---------------------------------------------------------------------------

static void testLoopbackSession(void)
{
    static const uint8_t kMask[4] = { 0x37, 0xFA, 0x21, 0x3D };
    Harness harness;
    DglabNetServerConfig config;
    char request[256];
    char frame[1024];
    uint8_t outgoing[512];
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int listener;
    int client;
    int server;
    int reuse = 1;
    int server_fd;
    WsConn conn;
    size_t request_len;

    memset(&harness, 0, sizeof(harness));

    memset(&config, 0, sizeof(config));
    config.port = DGLAB_NET_DEFAULT_PORT;
    config.now_ms = harnessNow;
    config.fill_random = harnessRandom;
    config.get_ip = harnessIp;
    config.context = &harness;

    dglabNetServerInit(&harness.server, &config);
    dglabNetServerSetListening(&harness.server, DGLAB_NET_DEFAULT_PORT);

    listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener < 0) {
        printf("SKIP loopback: cannot create a socket\n");
        return;
    }

    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        printf("SKIP loopback: cannot listen on loopback\n");
        close(listener);
        return;
    }

    CHECK(getsockname(listener, (struct sockaddr*)&address, &address_len) == 0);

    client = socket(AF_INET, SOCK_STREAM, 0);

    if (client < 0 || connect(client, (struct sockaddr*)&address, sizeof(address)) != 0) {
        printf("SKIP loopback: cannot connect\n");

        if (client >= 0)
            close(client);

        close(listener);
        return;
    }

    setTimeout(client);

    server = accept(listener, NULL, NULL);
    CHECK(server >= 0);

    if (server < 0) {
        close(client);
        close(listener);
        return;
    }

    setTimeout(server);

    server_fd = server;
    memset(&conn, 0, sizeof(conn));
    conn.read = socketRead;
    conn.write = socketWrite;
    conn.context = &server_fd;

    // The App scans the QR code and connects with the controller id in the path.
    request_len = (size_t)snprintf(request, sizeof(request),
        "GET /%s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        harness.server.controller_id);

    CHECK(sendAll(client, (const uint8_t*)request, request_len));
    CHECK(wsConnHandshake(&conn));
    CHECK(strcmp(conn.target, "/") != 0);
    CHECK(readHandshakeResponse(client));

    // Pairing: the App is bound and answers with the id it was given.
    CHECK(dglabNetServerAttach(&harness.server, &conn));
    CHECK(harness.server.status.paired == 1);

    CHECK(readServerFrame(client, frame, sizeof(frame)));
    CHECK(strstr(frame, "\"type\":\"bind\"") != NULL);
    CHECK(strstr(frame, harness.server.controller_id) != NULL);
    CHECK(strstr(frame, (const char*)harness.server.status.peer_id) != NULL);

    CHECK(readServerFrame(client, frame, sizeof(frame)));
    CHECK(strstr(frame, "\"type\":\"heartbeat\"") != NULL);

    // The App reports its current strengths over the real socket.
    {
        static const char report[] =
            "{\"type\":\"msg\",\"clientId\":\"c\",\"targetId\":\"c\","
            "\"message\":\"strength-12+34+100+120\"}";
        size_t size = buildClientFrame(outgoing, sizeof(outgoing), report, kMask);
        size_t payload_size = 0;
        uint8_t payload[WS_MAX_MESSAGE];
        WsOpcode opcode;

        CHECK(size > 0);
        CHECK(sendAll(client, outgoing, size));

        CHECK(wsConnRecv(&conn, &opcode, payload, sizeof(payload), &payload_size));
        CHECK(opcode == WsOpcode_Text);

        dglabNetServerOnMessage(&harness.server, &conn, (const char*)payload, payload_size);

        CHECK(harness.server.status.app_strength_a == 12);
        CHECK(harness.server.status.app_strength_b == 34);
        CHECK(harness.server.status.app_limit_a == 100);
        CHECK(harness.server.status.app_limit_b == 120);
    }

    // A command from the sysmodule side arrives at the App with a JSON escaped
    // payload, including the quotes a pulse command carries.
    {
        DglabNetSendRequest send;

        memset(&send, 0, sizeof(send));
        send.command = DglabNetCommand_SetStrength;
        send.channel = 1;
        send.value = 42;

        CHECK(dglabNetServerSend(&harness.server, &send) == DglabNetSend_Ok);
        CHECK(readServerFrame(client, frame, sizeof(frame)));
        CHECK(strstr(frame, "\"message\":\"strength-1+2+42\"") != NULL);

        send.command = DglabNetCommand_TestPulse;
        send.value = 10;

        CHECK(dglabNetServerSend(&harness.server, &send) == DglabNetSend_Ok);
        CHECK(readServerFrame(client, frame, sizeof(frame)));
        CHECK(strstr(frame, "\"message\":\"pulse-A:[\\\"") != NULL);
    }

    // A WebSocket ping is answered with a pong and counted, because that is how
    // the App keeps the link alive: without the counter the server sees a
    // perfectly healthy client as silent.
    {
        size_t size = buildClientControlFrame(outgoing, sizeof(outgoing), 0x9u, kMask);
        uint8_t ping_reply[WS_MAX_MESSAGE];
        size_t payload_size = 0;
        WsOpcode opcode;
        static const char after[] =
            "{\"type\":\"msg\",\"clientId\":\"c\",\"targetId\":\"c\",\"message\":\"feedback-1\"}";

        CHECK(size > 0);
        CHECK(sendAll(client, outgoing, size));

        // wsConnRecv keeps reading after handling the ping, so a data frame
        // follows it; the ping itself must have been counted by then.
        {
            size_t text_size = buildClientFrame(outgoing, sizeof(outgoing), after, kMask);

            CHECK(text_size > 0);
            CHECK(sendAll(client, outgoing, text_size));
        }

        CHECK(wsConnRecv(&conn, &opcode, ping_reply, sizeof(ping_reply), &payload_size));
        CHECK(opcode == WsOpcode_Text);
        CHECK(conn.ping_count == 1);

        dglabNetServerOnMessage(&harness.server, &conn, (const char*)ping_reply, payload_size);
        dglabNetServerOnActivity(&harness.server, &conn);
    }

    // Closing the App side makes the server drop the binding.
    close(client);

    {
        size_t payload_size = 0;
        uint8_t payload[WS_MAX_MESSAGE];
        WsOpcode opcode;

        CHECK(!wsConnRecv(&conn, &opcode, payload, sizeof(payload), &payload_size));
    }

    dglabNetServerDetach(&harness.server, &conn);

    CHECK(harness.server.status.paired == 0);
    CHECK(harness.server.status.state == DglabNetState_Listening);

    close(server);
    close(listener);
}

int main(void)
{
    testLoopbackSession();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
