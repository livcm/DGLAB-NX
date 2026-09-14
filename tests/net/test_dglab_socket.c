// Host side tests for the DG-LAB V3 socket protocol layer.
//
// Test data comes from docs/dglab-socket.md, which is based on the official
// dglab-websocket-server implementation and the PyDGLab-WS reference.
//
// Run with: make -C tests/net

#include <dglab/net/dglab_socket.h>

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

static void expectString(const char* name, const char* actual, const char* expected)
{
    g_checks++;

    if (strcmp(actual, expected) != 0) {
        printf("FAIL %s: got \"%s\", expected \"%s\"\n", name, actual, expected);
        g_failures++;
    }
}

// ---------------------------------------------------------------------------
// Envelope parsing
// ---------------------------------------------------------------------------

static void testParseBind(void)
{
    const char* text = "{\"type\":\"bind\",\"clientId\":\"11111111-2222-3333-4444-555555555555\","
                       "\"targetId\":\"\",\"message\":\"targetId\"}";
    DglabSocketMessage message;

    CHECK(dglabSocketParseMessage(text, strlen(text), &message));
    CHECK(message.type == DglabSocketType_Bind);
    expectString("clientId", message.client_id, "11111111-2222-3333-4444-555555555555");
    expectString("targetId", message.target_id, "");
    expectString("message", message.message, "targetId");
    CHECK(message.code == -1);
}

static void testParseCode(void)
{
    const char* text = "{\"type\":\"bind\",\"clientId\":\"a\",\"targetId\":\"b\",\"message\":\"200\"}";
    DglabSocketMessage message;

    CHECK(dglabSocketParseMessage(text, strlen(text), &message));
    CHECK(message.code == 200);

    const char* error = "{\"type\":\"error\",\"message\":\"405\"}";
    CHECK(dglabSocketParseMessage(error, strlen(error), &message));
    CHECK(message.type == DglabSocketType_Error);
    CHECK(message.code == 405);
}

static void testParseCommand(void)
{
    const char* text = "{\"type\":\"msg\",\"clientId\":\"a\",\"targetId\":\"b\","
                       "\"message\":\"strength-10+20+100+100\"}";
    DglabSocketMessage message;

    CHECK(dglabSocketParseMessage(text, strlen(text), &message));
    CHECK(message.type == DglabSocketType_Msg);
    expectString("command", message.message, "strength-10+20+100+100");
}

static void testParseTolerances(void)
{
    DglabSocketMessage message;

    // Key order may differ and whitespace is allowed.
    const char* text = "{ \"message\" : \"pulse-A:[\\\"0A0A0A0A00000000\\\"]\" , "
                       "\"targetId\" : \"b\" , \"type\" : \"msg\" , \"clientId\" : \"a\" }";
    CHECK(dglabSocketParseMessage(text, strlen(text), &message));
    CHECK(message.type == DglabSocketType_Msg);
    expectString("escaped pulse", message.message, "pulse-A:[\"0A0A0A0A00000000\"]");

    // Rejects: no type, not an object, unknown type.
    const char* no_type = "{\"clientId\":\"a\"}";
    CHECK(!dglabSocketParseMessage(no_type, strlen(no_type), &message));

    const char* not_object = "[]";
    CHECK(!dglabSocketParseMessage(not_object, strlen(not_object), &message));

    const char* unknown = "{\"type\":\"weird\"}";
    CHECK(dglabSocketParseMessage(unknown, strlen(unknown), &message));
    CHECK(message.type == DglabSocketType_Unknown);
}

static void testBuildMessage(void)
{
    char buffer[256];
    size_t length = dglabSocketBuildMessage(buffer, sizeof(buffer), "bind",
        "11111111-2222-3333-4444-555555555555", "", "targetId");

    CHECK(length > 0);
    expectString("built bind", buffer,
        "{\"type\":\"bind\",\"clientId\":\"11111111-2222-3333-4444-555555555555\","
        "\"targetId\":\"\",\"message\":\"targetId\"}");

    // A too small buffer must fail rather than truncate.
    char small[16];
    CHECK(dglabSocketBuildMessage(small, sizeof(small), "msg", "a", "b", "c") == 0);

    // The pulse command contains double quotes, so the envelope has to escape
    // them or the App receives broken JSON.
    char escaped[256];
    CHECK(dglabSocketBuildMessage(escaped, sizeof(escaped), "msg", "a", "b",
              "pulse-A:[\"0A0A0A0A00000000\"]") > 0);
    expectString("escaped envelope", escaped,
        "{\"type\":\"msg\",\"clientId\":\"a\",\"targetId\":\"b\","
        "\"message\":\"pulse-A:[\\\"0A0A0A0A00000000\\\"]\"}");

    CHECK(dglabSocketBuildMessage(escaped, sizeof(escaped), "msg", "a", "b", "back\\slash") > 0);
    expectString("escaped backslash", escaped,
        "{\"type\":\"msg\",\"clientId\":\"a\",\"targetId\":\"b\","
        "\"message\":\"back\\\\slash\"}");

    // Control characters cannot be represented and must be rejected.
    CHECK(dglabSocketBuildMessage(escaped, sizeof(escaped), "msg", "a", "b", "bad\nline") == 0);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void testCommands(void)
{
    char buffer[512];

    CHECK(dglabSocketBuildStrength(buffer, sizeof(buffer), DglabSocketChannel_A,
              DglabSocketStrength_Increase, 10) > 0);
    expectString("strength A +10", buffer, "strength-1+1+10");

    CHECK(dglabSocketBuildStrength(buffer, sizeof(buffer), DglabSocketChannel_B,
              DglabSocketStrength_SetTo, 200) > 0);
    expectString("strength B set 200", buffer, "strength-2+2+200");

    CHECK(dglabSocketBuildStrength(buffer, sizeof(buffer), DglabSocketChannel_A,
              DglabSocketStrength_Decrease, 5) > 0);
    expectString("strength A -5", buffer, "strength-1+0+5");

    CHECK(dglabSocketBuildClear(buffer, sizeof(buffer), DglabSocketChannel_A) > 0);
    expectString("clear A", buffer, "clear-1");
    CHECK(dglabSocketBuildClear(buffer, sizeof(buffer), DglabSocketChannel_B) > 0);
    expectString("clear B", buffer, "clear-2");
}

static void testPulseCommand(void)
{
    static const char* const pulses[] = { "0A0A0A0A00000000", "0A0A0A0A14141414" };
    char buffer[256];

    CHECK(dglabSocketBuildPulse(buffer, sizeof(buffer), DglabSocketChannel_A, pulses, 2) > 0);
    expectString("pulse A", buffer,
        "pulse-A:[\"0A0A0A0A00000000\",\"0A0A0A0A14141414\"]");

    CHECK(dglabSocketBuildPulse(buffer, sizeof(buffer), DglabSocketChannel_B, pulses, 1) > 0);
    expectString("pulse B", buffer, "pulse-B:[\"0A0A0A0A00000000\"]");

    char small[16];
    CHECK(dglabSocketBuildPulse(small, sizeof(small), DglabSocketChannel_A, pulses, 2) == 0);
}

static void testEncodePulseHex(void)
{
    const DglabCoyoteV3WaveformSlot slots[DGLAB_COYOTE_V3_WAVEFORM_SLOTS] = {
        { 10, 0 }, { 10, 20 }, { 20, 40 }, { 30, 50 },
    };
    char hex[17];

    dglabSocketEncodePulseHex(slots, hex);
    expectString("pulse hex", hex, "0A0A141E00142832");

    // Values taken from the official example data (呼吸 preset) to make sure the
    // byte order matches the App's expectation: frequencies first, then strengths.
    const DglabCoyoteV3WaveformSlot breath[DGLAB_COYOTE_V3_WAVEFORM_SLOTS] = {
        { 10, 100 }, { 10, 100 }, { 10, 100 }, { 10, 100 },
    };

    dglabSocketEncodePulseHex(breath, hex);
    expectString("breath pulse hex", hex, "0A0A0A0A64646464");
}

// ---------------------------------------------------------------------------
// Reports from the App
// ---------------------------------------------------------------------------

static void testStrengthReport(void)
{
    DglabSocketStrengthData data;

    CHECK(dglabSocketParseStrengthReport("strength-10+20+100+150", &data));
    CHECK(data.a == 10);
    CHECK(data.b == 20);
    CHECK(data.a_limit == 100);
    CHECK(data.b_limit == 150);

    CHECK(!dglabSocketParseStrengthReport("feedback-1", &data));
    CHECK(!dglabSocketParseStrengthReport("strength-10+20+100", &data));
    CHECK(!dglabSocketParseStrengthReport("strength-10+20+100+150+2", &data));
}

static void testFeedbackReport(void)
{
    int button = -1;

    CHECK(dglabSocketParseFeedback("feedback-4", &button));
    CHECK(button == 4);
    CHECK(dglabSocketParseFeedback("feedback-0", &button));
    CHECK(button == 0);
    CHECK(!dglabSocketParseFeedback("strength-1+2+3+4", &button));
    CHECK(!dglabSocketParseFeedback("feedback-", &button));
}

static void testQrUrl(void)
{
    char buffer[256];

    CHECK(dglabSocketBuildQrUrl(buffer, sizeof(buffer), "ws://192.168.1.161:9999",
              "11111111-2222-3333-4444-555555555555") > 0);
    expectString("qr url", buffer,
        "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#"
        "ws://192.168.1.161:9999/11111111-2222-3333-4444-555555555555");
}

int main(void)
{
    testParseBind();
    testParseCode();
    testParseCommand();
    testParseTolerances();
    testBuildMessage();
    testCommands();
    testPulseCommand();
    testEncodePulseHex();
    testStrengthReport();
    testFeedbackReport();
    testQrUrl();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
