// Host side tests for the CMIF layout rules used by the sysmodule IPC server.
//
// The sysmodule implements its IPC server by hand, so the request parsing and
// reply sizing in dglab/ipc_cmif.h have to agree with what libnx's client side
// does. These tests build requests with libnx's own cmifMakeRequest and feed
// them to the same helpers the sysmodule uses, which is the only way to check
// this contract without a console.
//
// Run with: make -C tests/ipc

#include <dglab/ipc_cmif.h>
#include <dglab/ipc.h>
#include <dglab/ipc_poc.h>

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

// Stands in for the thread local IPC buffer.
static u8 g_ipc_buffer[DGLAB_IPC_BUFFER_SIZE] __attribute__((aligned(16)));

// Copies `out` from the CMIF data area the way the sysmodule does and starts a
// new session, so every reply layout check uses the exact same helpers.
static const CmifOutHeader* buildReply(u32 payload_size, const void* payload, u32* num_data_words)
{
    u32 words = dglabResponseDataWords(payload_size);
    HipcRequest hipc;
    CmifOutHeader* out;

    memset(g_ipc_buffer, 0, sizeof(g_ipc_buffer));

    hipc = hipcMakeRequestInline(g_ipc_buffer,
        .type           = CmifCommandType_Request,
        .num_data_words = words,
    );

    out = (CmifOutHeader*)cmifGetAlignedDataStart(hipc.data_words, g_ipc_buffer);
    out->magic = CMIF_OUT_HEADER_MAGIC;
    out->version = 0;
    out->result = 0;
    out->token = 0;

    if (payload_size)
        memcpy(out + 1, payload, payload_size);

    if (num_data_words)
        *num_data_words = words;

    return out;
}

// Checks the reply the sysmodule would send for a given payload size: the whole
// payload has to sit inside the declared data area, otherwise the kernel copies
// only part of it and the client reads zeroed bytes.
static void checkReplyFits(const char* name, u32 payload_size)
{
    static u8 payload[0x100];
    u32 words = 0;
    const CmifOutHeader* out;
    u32 window;
    u32 aligned_offset;

    memset(payload, 0x5A, sizeof(payload));
    out = buildReply(payload_size, payload, &words);

    g_checks++;
    if (!dglabResponseFitsInline(payload_size)) {
        printf("FAIL %s: reply of %u bytes does not fit the IPC buffer\n", name, payload_size);
        g_failures++;
        return;
    }

    window = words * sizeof(u32);
    aligned_offset = (u32)((const u8*)out - (const u8*)g_ipc_buffer);

    g_checks++;
    if (window < aligned_offset + sizeof(CmifOutHeader) + payload_size) {
        printf("FAIL %s: data words %u cover %u bytes but %u are needed\n", name, words, window,
            aligned_offset + (u32)sizeof(CmifOutHeader) + payload_size);
        g_failures++;
    }

    CHECK(out->magic == CMIF_OUT_HEADER_MAGIC);
    CHECK(memcmp(out + 1, payload, payload_size) == 0);
}

// Builds a request exactly like libnx's serviceDispatchIn does for a non-domain
// service, then parses it the way the sysmodule does.
static void checkRequestRoundTrip(const char* name, u32 command_id, const void* payload,
    u32 payload_size)
{
    CmifRequest req;
    HipcParsedRequest parsed;
    const CmifInHeader* in;
    u8 received[64];

    CHECK(payload_size <= sizeof(received));

    memset(g_ipc_buffer, 0, sizeof(g_ipc_buffer));

    req = cmifMakeRequest(g_ipc_buffer, (CmifRequestFormat){
        .request_id = command_id,
        .data_size  = payload_size,
    });

    if (payload_size)
        memcpy(req.data, payload, payload_size);

    // Parse it exactly like the sysmodule's IPC server does.
    parsed = hipcParseRequest(g_ipc_buffer);
    in = (const CmifInHeader*)cmifGetAlignedDataStart(parsed.data.data_words, g_ipc_buffer);

    CHECK(parsed.meta.type == CmifCommandType_Request);
    CHECK(in->magic == CMIF_IN_HEADER_MAGIC);
    CHECK(in->command_id == command_id);

    // The server must accept the request ...
    g_checks++;
    if (!dglabRequestHasPayload(parsed.meta.num_data_words, payload_size)) {
        printf("FAIL %s: server rejected a well formed request (%u words, %u bytes)\n", name,
            parsed.meta.num_data_words, payload_size);
        g_failures++;
        return;
    }

    // ... and read the payload from the offset the client wrote it to.
    memset(received, 0, sizeof(received));

    if (payload_size)
        memcpy(received, dglabRequestPayload(in), payload_size);

    g_checks++;
    if (memcmp(received, payload, payload_size) != 0) {
        printf("FAIL %s: payload mismatch\n", name);
        g_failures++;
    }
}

static void testRequestPayloads(void)
{
    const DglabPocStartRequest start = { .applet_resource_user_id = 0x0123456789ABCDEFull };
    const DglabPocActionRequest action = { .action = DglabPocAction_WriteZeroB0 };
    const DglabPocLogRequest log = { .cursor = 0x00001234u };

    checkRequestRoundTrip("start", DGLAB_IPC_POC_CMD_START, &start, sizeof(start));
    checkRequestRoundTrip("action", DGLAB_IPC_POC_CMD_ACTION, &action, sizeof(action));
    checkRequestRoundTrip("log", DGLAB_IPC_POC_CMD_LOG, &log, sizeof(log));
}

// A command without a payload must not satisfy a check for one, otherwise the
// server would read stale bytes from the IPC buffer.
static void testMissingPayloadIsRejected(void)
{
    HipcParsedRequest parsed;

    memset(g_ipc_buffer, 0, sizeof(g_ipc_buffer));

    (void)cmifMakeRequest(g_ipc_buffer, (CmifRequestFormat){
        .request_id = DGLAB_IPC_CMD_PING,
        .data_size  = 0,
    });

    parsed = hipcParseRequest(g_ipc_buffer);

    CHECK(!dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(DglabPocActionRequest)));
    CHECK(!dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(DglabPocStartRequest)));
    CHECK(!dglabRequestHasPayload(parsed.meta.num_data_words, 8));

    // A request that carries an 8-byte payload satisfies an 8-byte read but not
    // a larger one.
    memset(g_ipc_buffer, 0, sizeof(g_ipc_buffer));

    (void)cmifMakeRequest(g_ipc_buffer, (CmifRequestFormat){
        .request_id = DGLAB_IPC_POC_CMD_START,
        .data_size  = sizeof(DglabPocStartRequest),
    });

    parsed = hipcParseRequest(g_ipc_buffer);

    CHECK(dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(DglabPocStartRequest)));
    CHECK(!dglabRequestHasPayload(parsed.meta.num_data_words, 64));
}

static void testReplySizes(void)
{
    checkReplyFits("ping", sizeof(u32));
    checkReplyFits("version", 12);
    checkReplyFits("status", sizeof(DglabPocStatus));
    checkReplyFits("log chunk", sizeof(DglabPocLogChunk));
    checkReplyFits("empty", 0);

    // The PoC structs must stay small enough for the inline data area.
    CHECK(sizeof(DglabPocStatus) <= DGLAB_IPC_INLINE_PAYLOAD_MAX);
    CHECK(sizeof(DglabPocLogChunk) <= DGLAB_IPC_INLINE_PAYLOAD_MAX);
}

// The waveform upload is the largest request on this IPC surface, and the only
// one that comes close to the buffer: 16 bytes of alignment, the 16 byte
// CmifInHeader and 208 bytes of payload fill the 0x100 byte IPC buffer exactly.
// An off by one in the accounting would show up here and nowhere else, so it is
// checked on its own instead of through the small payloads above.
static void testWaveformRequest(void)
{
    static DglabNetWaveformRequest request;
    static u8 received[sizeof(DglabNetWaveformRequest)];
    HipcParsedRequest parsed;
    const CmifInHeader* in;
    CmifRequest req;

    CHECK(DGLAB_CMIF_DATA_ALIGN + sizeof(CmifInHeader) + sizeof(request) <= DGLAB_IPC_BUFFER_SIZE);

    memset(&request, 0, sizeof(request));
    request.channel = 1;
    request.mode = DglabNetWaveform_Replace;
    request.slot_count = DGLAB_NET_WAVEFORM_MAX_SLOTS;

    for (u32 i = 0; i < request.slot_count; i++) {
        request.slots[i].frequency_ms = 100;
        request.slots[i].strength = 100;
    }

    memset(g_ipc_buffer, 0, sizeof(g_ipc_buffer));

    req = cmifMakeRequest(g_ipc_buffer, (CmifRequestFormat){
        .request_id = DGLAB_IPC_CMD_NET_WAVEFORM,
        .data_size  = sizeof(request),
    });

    memcpy(req.data, &request, sizeof(request));

    parsed = hipcParseRequest(g_ipc_buffer);
    in = (const CmifInHeader*)cmifGetAlignedDataStart(parsed.data.data_words, g_ipc_buffer);

    CHECK(parsed.meta.type == CmifCommandType_Request);
    CHECK(in->magic == CMIF_IN_HEADER_MAGIC);
    CHECK(in->command_id == DGLAB_IPC_CMD_NET_WAVEFORM);
    CHECK(dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(request)));

    memset(received, 0, sizeof(received));
    memcpy(received, dglabRequestPayload(in), sizeof(received));
    CHECK(memcmp(received, &request, sizeof(request)) == 0);
}

int main(void)
{
    testRequestPayloads();
    testWaveformRequest();
    testMissingPayloadIsRejected();
    testReplySizes();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
