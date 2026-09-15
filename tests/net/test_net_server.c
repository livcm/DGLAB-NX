// Host side tests for the DG-LAB Socket V3 server core.
//
// The transport is replaced by an in-memory WebSocket connection: reads are
// never needed here (the transport thread does them on the Switch), and every
// write is captured so the frames sent to the App can be checked.
//
// Run with: make -C tests/net

#include <dglab/net/net_server.h>

#include <dglab/net/dglab_socket.h>
#include <dglab/protocol/coyote_v3.h>

#include <stdio.h>
#include <string.h>

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
// In-memory transport
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t tx[8192];
    size_t tx_size;
} MockLink;

static int mockWrite(void* context, const uint8_t* buffer, size_t size)
{
    MockLink* link = context;

    if (link->tx_size + size > sizeof(link->tx))
        return -1;

    memcpy(link->tx + link->tx_size, buffer, size);
    link->tx_size += size;

    return (int)size;
}

static int mockRead(void* context, uint8_t* buffer, size_t size)
{
    (void)context;
    (void)buffer;
    (void)size;

    return 0;
}

static void linkInit(WsConn* conn, MockLink* link, const char* target)
{
    memset(link, 0, sizeof(*link));
    memset(conn, 0, sizeof(*conn));

    conn->read = mockRead;
    conn->write = mockWrite;
    conn->context = link;
    snprintf(conn->target, sizeof(conn->target), "%s", target);
}

// Copies the payload of the nth frame into out. Returns false when there is no
// such frame.
static bool payloadAt(const MockLink* link, size_t index, char* out, size_t out_size)
{
    size_t offset = 0;

    out[0] = '\0';

    for (size_t frame = 0; offset + 2 <= link->tx_size; frame++) {
        size_t header = 2;
        size_t len = link->tx[offset + 1] & 0x7Fu;

        if (len == 126) {
            if (offset + 4 > link->tx_size)
                return false;

            len = ((size_t)link->tx[offset + 2] << 8) | link->tx[offset + 3];
            header = 4;
        } else if (len == 127) {
            return false; // never used by a message this small
        }

        if (offset + header + len > link->tx_size)
            return false;

        if (frame == index) {
            if (len >= out_size)
                return false;

            memcpy(out, link->tx + offset + header, len);
            out[len] = '\0';

            return true;
        }

        offset += header + len;
    }

    return false;
}

static bool contains(const char* haystack, const char* needle)
{
    return strstr(haystack, needle) != NULL;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

typedef struct {
    DglabNetServer server;
    uint64_t now;
    uint32_t random_calls;
    bool random_ok;
    bool ip_ok;
} Harness;

static uint64_t harnessNow(void* context)
{
    return ((Harness*)context)->now;
}

static bool harnessRandom(void* context, uint8_t* out, size_t size)
{
    Harness* harness = context;

    if (!harness->random_ok)
        return false;

    harness->random_calls++;

    for (size_t i = 0; i < size; i++)
        out[i] = (uint8_t)(harness->random_calls * 16u + (uint32_t)i);

    return true;
}

static bool harnessIp(void* context, uint32_t* address, char* text, size_t text_size)
{
    Harness* harness = context;

    if (!harness->ip_ok)
        return false;

    snprintf(text, text_size, "192.168.1.50");
    *address = 0xC0A80132u;

    return true;
}

static void harnessInit(Harness* harness)
{
    DglabNetServerConfig config;

    memset(harness, 0, sizeof(*harness));
    harness->random_ok = true;
    harness->ip_ok = true;

    memset(&config, 0, sizeof(config));
    config.port = DGLAB_NET_DEFAULT_PORT;
    config.now_ms = harnessNow;
    config.fill_random = harnessRandom;
    config.get_ip = harnessIp;
    config.context = harness;

    dglabNetServerInit(&harness->server, &config);
}

// Attaches one client that presents the server's controller id. Returns the
// captured link.
static bool attachApp(Harness* harness, WsConn* conn, MockLink* link)
{
    char target[64];

    snprintf(target, sizeof(target), "/%s", harness->server.controller_id);
    linkInit(conn, link, target);

    return dglabNetServerAttach(&harness->server, conn);
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

static void testUuid(void)
{
    static const uint8_t bytes[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    char buffer[40];

    CHECK(dglabNetFormatUuid(bytes, buffer, sizeof(buffer)) == 36);
    CHECK(strcmp(buffer, "00112233-4455-6677-8899-aabbccddeeff") == 0);

    // Exactly the room a uuid needs is enough, one byte less is not.
    CHECK(dglabNetFormatUuid(bytes, buffer, 37) == 36);
    CHECK(dglabNetFormatUuid(bytes, buffer, 36) == 0);
}

static void testTargetId(void)
{
    char buffer[64];

    CHECK(dglabNetParseTargetId("/abc", buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "abc") == 0);

    CHECK(dglabNetParseTargetId("/abc?x=1", buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "abc") == 0);

    CHECK(dglabNetParseTargetId("/?tid=xyz", buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "xyz") == 0);

    CHECK(dglabNetParseTargetId("?tid=xyz&other=1", buffer, sizeof(buffer)));
    CHECK(strcmp(buffer, "xyz") == 0);

    CHECK(!dglabNetParseTargetId("", buffer, sizeof(buffer)));
    CHECK(!dglabNetParseTargetId("/", buffer, sizeof(buffer)));
    CHECK(!dglabNetParseTargetId("/?x=1", buffer, sizeof(buffer)));

    // A value that does not fit is refused instead of truncated.
    CHECK(!dglabNetParseTargetId("/abcdef", buffer, 4));
}

// ---------------------------------------------------------------------------
// Lifecycle and pairing
// ---------------------------------------------------------------------------

static void testPairing(void)
{
    Harness harness;
    WsConn conn;
    MockLink link;
    char frame[4096];
    char target[64];

    harnessInit(&harness);

    CHECK(harness.server.status.state == DglabNetState_Idle);
    CHECK(harness.server.status.app_feedback == DGLAB_NET_FEEDBACK_NONE);
    CHECK(strlen(harness.server.controller_id) == 36);
    CHECK(harness.server.controller_id[14] == '4'); // uuid version 4
    CHECK(strchr("89ab", harness.server.controller_id[19]) != NULL); // RFC 4122 variant

    dglabNetServerSetListening(&harness.server, 9999);
    CHECK(harness.server.status.state == DglabNetState_Listening);
    CHECK(harness.server.status.port == 9999);

    // A target carrying a foreign id is paired anyway: real devices turned out
    // not to send the id the reference implementation expects, and rejecting
    // leaves the App spinning in "connecting". The mismatch has to be logged,
    // because that log is what the pairing rule gets tightened against.
    linkInit(&conn, &link, "/11111111-1111-4111-8111-111111111111");
    CHECK(dglabNetServerAttach(&harness.server, &conn));
    CHECK(harness.server.status.paired == 1);

    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"type\":\"bind\""));
    CHECK(contains(frame, "\"message\":\"200\""));

    {
        char log[1024];

        dglabNetServerReadLog(&harness.server, 0, log, sizeof(log));
        CHECK(contains(log, "11111111-1111-4111-8111-111111111111"));
        CHECK(contains(log, "does not match"));
    }

    dglabNetServerDetach(&harness.server, &conn);
    CHECK(harness.server.status.paired == 0);

    // A connection without any id in the target is paired as well, and logged.
    linkInit(&conn, &link, "/");
    CHECK(dglabNetServerAttach(&harness.server, &conn));
    CHECK(harness.server.status.paired == 1);
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"message\":\"200\""));

    {
        char log[1024];

        dglabNetServerReadLog(&harness.server, 0, log, sizeof(log));
        CHECK(contains(log, "without a client id"));
    }

    dglabNetServerDetach(&harness.server, &conn);

    // The QR code carries the controller id, and that is what the App sends.
    {
        u32 beats_before = harness.server.status.heartbeats_sent;

    CHECK(attachApp(&harness, &conn, &link));
    CHECK(harness.server.status.state == DglabNetState_Paired);
    CHECK(harness.server.status.clients == 1);
    CHECK(harness.server.status.paired == 1);
    CHECK(strlen((const char*)harness.server.status.peer_id) == 36);
    CHECK(strcmp((const char*)harness.server.status.peer_id, harness.server.controller_id) != 0);

    // Frame 0 is the bind reply: clientId is the controller, targetId is the
    // App's own id, which is how the App learns its id.
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"type\":\"bind\""));
    CHECK(contains(frame, harness.server.controller_id));
    CHECK(contains(frame, (const char*)harness.server.status.peer_id));
    CHECK(contains(frame, "\"message\":\"200\""));

    // Frame 1 is the heartbeat that goes out right after binding.
    CHECK(payloadAt(&link, 1, frame, sizeof(frame)));
    CHECK(contains(frame, "\"type\":\"heartbeat\""));
        CHECK(harness.server.status.heartbeats_sent == beats_before + 1);
    }

    // Only one client can be bound at a time (400 for the second one).
    {
        WsConn second;
        MockLink second_link;

        snprintf(target, sizeof(target), "/%s", harness.server.controller_id);
        linkInit(&second, &second_link, target);

        CHECK(!dglabNetServerAttach(&harness.server, &second));
        CHECK(payloadAt(&second_link, 0, frame, sizeof(frame)));
        CHECK(contains(frame, "\"message\":\"400\""));
        CHECK(harness.server.status.clients == 1);
    }

    // Every handshaked connection is counted, accepted or not.
    CHECK(harness.server.status.sessions == 4);

    dglabNetServerDetach(&harness.server, &conn);
    CHECK(harness.server.status.paired == 0);
    CHECK(harness.server.status.clients == 0);
    CHECK(harness.server.status.state == DglabNetState_Listening);
    CHECK(harness.server.status.peer_id[0] == '\0');
}

// ---------------------------------------------------------------------------
// Messages from the App
// ---------------------------------------------------------------------------

static void testAppMessages(void)
{
    Harness harness;
    WsConn conn;
    MockLink link;
    char frame[4096];
    static const char report[] =
        "{\"type\":\"msg\",\"clientId\":\"c\",\"targetId\":\"c\",\"message\":\"strength-10+20+150+160\"}";
    static const char feedback[] =
        "{\"type\":\"msg\",\"clientId\":\"c\",\"targetId\":\"c\",\"message\":\"feedback-3\"}";
    static const char beat[] = "{\"type\":\"heartbeat\",\"clientId\":\"c\",\"targetId\":\"c\",\"message\":\"200\"}";
    static const char unknown[] =
        "{\"type\":\"msg\",\"clientId\":\"c\",\"targetId\":\"c\",\"message\":\"future-command\"}";
    static const char broken[] = "not json at all";

    harnessInit(&harness);
    dglabNetServerSetListening(&harness.server, 9999);
    CHECK(attachApp(&harness, &conn, &link));

    dglabNetServerOnMessage(&harness.server, &conn, report, strlen(report));
    CHECK(harness.server.status.app_strength_a == 10);
    CHECK(harness.server.status.app_strength_b == 20);
    CHECK(harness.server.status.app_limit_a == 150);
    CHECK(harness.server.status.app_limit_b == 160);
    CHECK(harness.server.status.reports_received == 1);
    CHECK(harness.server.status.messages_in == 1);

    dglabNetServerOnMessage(&harness.server, &conn, feedback, strlen(feedback));
    CHECK(harness.server.status.app_feedback == 3);
    CHECK(harness.server.status.reports_received == 2);

    // Heartbeats only refresh liveness: no reply frame and no error.
    link.tx_size = 0;
    dglabNetServerOnMessage(&harness.server, &conn, beat, strlen(beat));
    CHECK(harness.server.status.messages_in == 3);
    CHECK(link.tx_size == 0);

    // Payloads this build does not know are logged, not rejected.
    dglabNetServerOnMessage(&harness.server, &conn, unknown, strlen(unknown));
    CHECK(link.tx_size == 0);
    CHECK(harness.server.status.last_error == 0);

    // A body that is not JSON is answered with 403.
    dglabNetServerOnMessage(&harness.server, &conn, broken, strlen(broken));
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"message\":\"403\""));

    // Anything past the 1950 byte socket limit is answered with 405.
    {
        char big[DGLAB_SOCKET_MAX_MESSAGE + 2];

        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';

        link.tx_size = 0;
        dglabNetServerOnMessage(&harness.server, &conn, big, sizeof(big) - 1);
        CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
        CHECK(contains(frame, "\"message\":\"405\""));
        CHECK(harness.server.status.last_error == 405);
    }

    // The log keeps the traffic visible for the NRO.
    {
        char log[512];
        dglabNetServerReadLog(&harness.server, 0, log, sizeof(log));
        CHECK(contains(log, "app "));
        CHECK(contains(log, "strength-10+20+150+160"));
        CHECK(contains(log, "unhandled app message: future-command"));
    }

    dglabNetServerDetach(&harness.server, &conn);
}

// ---------------------------------------------------------------------------
// Commands to the App
// ---------------------------------------------------------------------------

static void testCommands(void)
{
    Harness harness;
    WsConn conn;
    MockLink link;
    char frame[4096];
    char expected[512];
    DglabNetSendRequest request;

    harnessInit(&harness);
    dglabNetServerSetListening(&harness.server, 9999);

    memset(&request, 0, sizeof(request));
    request.command = DglabNetCommand_SetStrength;
    request.channel = 1;
    request.value = 50;

    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_NotPaired);

    CHECK(attachApp(&harness, &conn, &link));
    link.tx_size = 0;

    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_Ok);
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    // clientId is the sender (us) and targetId the recipient (the App): the App
    // ignores a message addressed the other way round.
    snprintf(expected, sizeof(expected),
        "{\"type\":\"msg\",\"clientId\":\"%s\",\"targetId\":\"%s\",\"message\":\"strength-1+2+50\"}",
        harness.server.controller_id, (const char*)harness.server.status.peer_id);
    CHECK(strcmp(frame, expected) == 0);
    CHECK(harness.server.status.commands_sent == 1);

    // Channel 0 addresses both channels, so two commands go out.
    request.channel = 0;
    link.tx_size = 0;
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_Ok);
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"message\":\"strength-1+2+50\""));
    CHECK(payloadAt(&link, 1, frame, sizeof(frame)));
    CHECK(contains(frame, "\"message\":\"strength-2+2+50\""));

    // Out of range values and channels are refused before anything is sent.
    request.channel = 1;
    request.value = DGLAB_COYOTE_V3_STRENGTH_MAX + 1;
    link.tx_size = 0;
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_BadRequest);
    CHECK(link.tx_size == 0);

    request.value = 50;
    request.channel = 3;
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_BadRequest);

    request.command = DglabNetCommand_Clear;
    request.channel = 2;
    link.tx_size = 0;
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_Ok);
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"message\":\"clear-2\""));

    request.command = DglabNetCommand_TestPulse;
    request.channel = 1;
    request.value = 20;
    link.tx_size = 0;
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_Ok);
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"message\":\"pulse-A:[\\\""));
    // Eight 8 byte elements, each written as 16 hex characters.
    {
        int quotes = 0;

        for (const char* p = frame; *p; p++) {
            if (strncmp(p, "\\\"", 2) == 0)
                quotes++;
        }

        CHECK(quotes == 16);
    }

    request.value = 0; // 0 selects the default test strength
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_Ok);

    request.value = DGLAB_COYOTE_V3_WAVEFORM_STRENGTH_MAX + 1;
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_BadRequest);

    request.command = 42;
    request.value = 0;
    CHECK(dglabNetServerSend(&harness.server, &request) == DglabNetSend_BadRequest);

    dglabNetServerDetach(&harness.server, &conn);
}

// ---------------------------------------------------------------------------
// Heartbeat and address
// ---------------------------------------------------------------------------

static void testHeartbeatAndQr(void)
{
    Harness harness;
    WsConn conn;
    MockLink link;
    char frame[4096];
    char qr[DGLAB_NET_QR_MAX];
    size_t qr_size = 0;
    DglabNetStatus status;

    harnessInit(&harness);
    dglabNetServerSetListening(&harness.server, 9999);
    CHECK(attachApp(&harness, &conn, &link));

    // The bind reply and the first heartbeat are already out.
    CHECK(harness.server.status.heartbeats_sent == 1);

    link.tx_size = 0;
    harness.now += DGLAB_NET_HEARTBEAT_INTERVAL_MS / 2u;
    dglabNetServerPoll(&harness.server, harness.now);
    CHECK(link.tx_size == 0);

    harness.now += DGLAB_NET_HEARTBEAT_INTERVAL_MS / 2u;
    dglabNetServerPoll(&harness.server, harness.now);
    CHECK(payloadAt(&link, 0, frame, sizeof(frame)));
    CHECK(contains(frame, "\"type\":\"heartbeat\""));
    CHECK(harness.server.status.heartbeats_sent == 2);

    dglabNetServerGetStatus(&harness.server, &status);
    CHECK(strcmp((const char*)status.ip_text, "192.168.1.50") == 0);
    CHECK(status.ip == 0xC0A80132u);

    CHECK(dglabNetServerGetQr(&harness.server, qr, sizeof(qr), &qr_size));
    CHECK(qr_size == strlen(qr));

    {
        char expected[DGLAB_NET_QR_MAX];

        snprintf(expected, sizeof(expected),
            "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#"
            "ws://192.168.1.50:9999/%s",
            harness.server.controller_id);

        CHECK(strcmp(qr, expected) == 0);
    }

    // Without a network address there is no QR payload to show.
    harness.ip_ok = false;
    CHECK(!dglabNetServerGetQr(&harness.server, qr, sizeof(qr), &qr_size));
    CHECK(qr_size == 0);
    CHECK(qr[0] == '\0');

    dglabNetServerGetStatus(&harness.server, &status);
    CHECK(status.ip_text[0] == '\0');
    CHECK(status.ip == 0);

    dglabNetServerDetach(&harness.server, &conn);
}

// ---------------------------------------------------------------------------
// Log ring and lifecycle transitions
// ---------------------------------------------------------------------------

static void testLogAndLifecycle(void)
{
    Harness harness;
    char log[256];
    u32 cursor;

    harnessInit(&harness);
    dglabNetServerSetListening(&harness.server, 9999);
    dglabNetServerSetStopped(&harness.server);

    CHECK(harness.server.status.state == DglabNetState_Stopped);

    dglabNetServerSetFailed(&harness.server, 0x1234);
    CHECK(harness.server.status.state == DglabNetState_Failed);
    CHECK(harness.server.status.last_result == 0x1234);

    dglabNetServerLog(&harness.server, "hello %u", 7u);

    cursor = dglabNetServerReadLog(&harness.server, 0, log, sizeof(log));
    CHECK(contains(log, "hello 7"));
    CHECK(cursor > 0);

    // Reading again from the returned cursor yields nothing new.
    {
        u32 next = dglabNetServerReadLog(&harness.server, cursor, log, sizeof(log));
        CHECK(log[0] == '\0');
        CHECK(next == cursor);
    }

    // A cursor from before the ring start is clamped instead of failing.
    dglabNetServerLog(&harness.server, "second line");
    CHECK(dglabNetServerReadLog(&harness.server, cursor, log, sizeof(log)) > cursor);
    CHECK(contains(log, "second line"));
}

// ---------------------------------------------------------------------------
// Fallback id
// ---------------------------------------------------------------------------

static void testFallbackId(void)
{
    Harness harness;
    WsConn conn;
    MockLink link;
    char first[DGLAB_NET_ID_LEN];
    char second[DGLAB_NET_ID_LEN];

    harnessInit(&harness);
    harness.random_ok = false;

    // Re-initialising without a random source still yields usable, distinct ids.
    dglabNetServerInit(&harness.server, &harness.server.config);
    snprintf(first, sizeof(first), "%s", harness.server.controller_id);

    dglabNetServerSetListening(&harness.server, 9999);
    CHECK(attachApp(&harness, &conn, &link));

    snprintf(second, sizeof(second), "%s", (const char*)harness.server.status.peer_id);

    CHECK(strlen(first) == 36);
    CHECK(strlen(second) == 36);
    CHECK(strcmp(first, second) != 0);

    dglabNetServerDetach(&harness.server, &conn);
}

int main(void)
{
    testUuid();
    testTargetId();
    testPairing();
    testAppMessages();
    testCommands();
    testHeartbeatAndQr();
    testLogAndLifecycle();
    testFallbackId();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
