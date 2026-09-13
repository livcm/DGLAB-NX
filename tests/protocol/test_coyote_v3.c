// Host side tests for the DG-LAB Coyote V3 protocol layer.
//
// The B0 vectors below are byte-for-byte copies of the examples published in
// coyote/v3/README.md of the official protocol repository, so a change that
// breaks them is a change that no longer matches the documented protocol.
//
// Run with: make -C tests/protocol

#include <dglab/protocol/coyote_v3.h>

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(condition)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_U8(actual, expected)                                          \
    do {                                                                    \
        unsigned int check_actual = (unsigned int)(actual);                 \
        unsigned int check_expected = (unsigned int)(expected);             \
        g_checks++;                                                         \
        if (check_actual != check_expected) {                               \
            printf("FAIL %s:%d: %s = 0x%02X, expected 0x%02X\n", __FILE__,  \
                __LINE__, #actual, check_actual, check_expected);           \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static bool bytesFromHex(const char* hex, uint8_t* out, size_t out_size)
{
    if (strlen(hex) != out_size * 2)
        return false;

    for (size_t i = 0; i < out_size; i++) {
        unsigned int value = 0;
        if (sscanf(hex + i * 2, "%2x", &value) != 1)
            return false;
        out[i] = (uint8_t)value;
    }

    return true;
}

static void printHex(const char* label, const uint8_t* bytes, size_t size)
{
    printf("  %s = ", label);
    for (size_t i = 0; i < size; i++)
        printf("%02X", bytes[i]);
    printf("\n");
}

static void expectBytes(const char* name, const uint8_t* actual, const char* expected_hex,
    size_t size)
{
    uint8_t expected[DGLAB_COYOTE_V3_B0_SIZE];

    if (!bytesFromHex(expected_hex, expected, size)) {
        printf("FAIL %s: malformed test vector '%s'\n", name, expected_hex);
        g_failures++;
        return;
    }

    g_checks++;
    if (memcmp(actual, expected, size) != 0) {
        printf("FAIL %s: packet mismatch\n", name);
        printHex("actual  ", actual, size);
        printHex("expected", expected, size);
        g_failures++;
    }
}

static void expectWaveformEquals(const char* name, const DglabCoyoteV3WaveformSlot* actual,
    const DglabCoyoteV3WaveformSlot* expected)
{
    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++) {
        g_checks++;
        if (actual[i].frequency != expected[i].frequency ||
            actual[i].strength != expected[i].strength) {
            printf("FAIL %s: slot %zu is {%u,%u}, expected {%u,%u}\n", name, i,
                (unsigned int)actual[i].frequency, (unsigned int)actual[i].strength,
                (unsigned int)expected[i].frequency, (unsigned int)expected[i].strength);
            g_failures++;
        }
    }
}

// Vectors copied verbatim from coyote/v3/README.md.
typedef struct {
    const char* name;
    const char* expected_hex;
    DglabCoyoteV3B0 packet;
} B0Vector;

static const B0Vector kB0Vectors[] = {
    {
        // "No.1 不修改通道强度，A 通道连续输出波形"
        .name = "official B0 no.1 (channel A only)",
        .expected_hex = "B00000000A0A0A0A000A141E0000000000000065",
        .packet = {
            .sequence = 0,
            .strength_a = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .strength_b = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .waveform_a = { { 10, 0 }, { 10, 10 }, { 10, 20 }, { 10, 30 } },
            .waveform_b = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 101 } },
        },
    },
    {
        // "No.2 ... 1 ->": A strength increases by 5 with sequence 0.
        .name = "official B0 no.2 (relative +5, sequence 0)",
        .expected_hex = "B00405000A0A0A0A000A141E0000000000000065",
        .packet = {
            .sequence = 0,
            .strength_a = { DglabCoyoteV3StrengthMode_RelativeIncrease, 5 },
            .strength_b = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .waveform_a = { { 10, 0 }, { 10, 10 }, { 10, 20 }, { 10, 30 } },
            .waveform_b = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 101 } },
        },
    },
    {
        // "No.2 ... 4 ->": A strength increases by 10 with sequence 1.
        .name = "official B0 no.2 (relative +10, sequence 1)",
        .expected_hex = "B0140A00283C5064645A5A5A0000000000000065",
        .packet = {
            .sequence = 1,
            .strength_a = { DglabCoyoteV3StrengthMode_RelativeIncrease, 10 },
            .strength_b = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .waveform_a = { { 40, 100 }, { 60, 90 }, { 80, 90 }, { 100, 90 } },
            .waveform_b = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 101 } },
        },
    },
    {
        // "No.4 ... 1 ->": both channels output waveform.
        .name = "official B0 no.4 (both channels)",
        .expected_hex = "B00000000A0A0A0A000A141E0A0A0A0A00000000",
        .packet = {
            .sequence = 0,
            .strength_a = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .strength_b = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .waveform_a = { { 10, 0 }, { 10, 10 }, { 10, 20 }, { 10, 30 } },
            .waveform_b = { { 10, 0 }, { 10, 0 }, { 10, 0 }, { 10, 0 } },
        },
    },
    {
        // "No.4 ... 3 ->": channel B ends with a non-zero strength.
        .name = "official B0 no.4 (channel B tail)",
        .expected_hex = "B00000001E1E1E1E505A64640A0A0A0A0000000A",
        .packet = {
            .sequence = 0,
            .strength_a = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .strength_b = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .waveform_a = { { 30, 80 }, { 30, 90 }, { 30, 100 }, { 30, 100 } },
            .waveform_b = { { 10, 0 }, { 10, 0 }, { 10, 0 }, { 10, 10 } },
        },
    },
    {
        // "No.4 ... 4 ->": channel A starts at strength 0.
        .name = "official B0 no.4 (channel A zero start)",
        .expected_hex = "B0000000283C5064005A5A5A0A0A0A0A0000000A",
        .packet = {
            .sequence = 0,
            .strength_a = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .strength_b = { DglabCoyoteV3StrengthMode_NoChange, 0 },
            .waveform_a = { { 40, 0 }, { 60, 90 }, { 80, 90 }, { 100, 90 } },
            .waveform_b = { { 10, 0 }, { 10, 0 }, { 10, 0 }, { 10, 10 } },
        },
    },
};

static void testOfficialB0Vectors(void)
{
    for (size_t i = 0; i < sizeof(kB0Vectors) / sizeof(kB0Vectors[0]); i++) {
        const B0Vector* vector = &kB0Vectors[i];
        uint8_t encoded[DGLAB_COYOTE_V3_B0_SIZE];

        dglabCoyoteV3EncodeB0(&vector->packet, encoded);
        expectBytes(vector->name, encoded, vector->expected_hex, sizeof(encoded));

        // Every official vector must also survive a decode/encode round trip.
        DglabCoyoteV3B0 decoded = { 0 };
        CHECK(dglabCoyoteV3DecodeB0(encoded, sizeof(encoded), &decoded));
        CHECK_U8(decoded.sequence, vector->packet.sequence);
        CHECK(decoded.strength_a.mode == vector->packet.strength_a.mode);
        CHECK(decoded.strength_b.mode == vector->packet.strength_b.mode);
        CHECK_U8(decoded.strength_a.value, vector->packet.strength_a.value);
        CHECK_U8(decoded.strength_b.value, vector->packet.strength_b.value);
        expectWaveformEquals(vector->name, decoded.waveform_a, vector->packet.waveform_a);
        expectWaveformEquals(vector->name, decoded.waveform_b, vector->packet.waveform_b);
    }
}

static void testStrengthModePacking(void)
{
    // The documented example for strengthParsingMethod 0b1101: channel A
    // absolute 5, channel B relative increase 8.
    DglabCoyoteV3B0 packet = { 0 };
    uint8_t encoded[DGLAB_COYOTE_V3_B0_SIZE];

    packet.strength_a.mode = DglabCoyoteV3StrengthMode_Absolute;
    packet.strength_a.value = 5;
    packet.strength_b.mode = DglabCoyoteV3StrengthMode_RelativeIncrease;
    packet.strength_b.value = 8;

    dglabCoyoteV3EncodeB0(&packet, encoded);

    // 0b0000 (sequence) | 0b11 (A absolute) | 0b01 (B increase) = 0x0D.
    CHECK_U8(encoded[1], 0x0D);
    CHECK_U8(encoded[2], 5);
    CHECK_U8(encoded[3], 8);

    // The sequence number occupies 4 bits only; higher bits must not leak into
    // the strength mode bits.
    packet.sequence = 0x12;
    dglabCoyoteV3EncodeB0(&packet, encoded);
    CHECK_U8(encoded[1], 0x2D);

    packet.sequence = 0x0F;
    dglabCoyoteV3EncodeB0(&packet, encoded);
    CHECK_U8(encoded[1], 0xFD);
}

static void testEncodeDoesNotOverrun(void)
{
    uint8_t buffer[DGLAB_COYOTE_V3_B0_SIZE + 4];
    DglabCoyoteV3B0 packet = { 0 };

    memset(buffer, 0xAA, sizeof(buffer));
    dglabCoyoteV3EncodeB0(&packet, buffer);

    for (size_t i = DGLAB_COYOTE_V3_B0_SIZE; i < sizeof(buffer); i++)
        CHECK_U8(buffer[i], 0xAA);

    uint8_t bf_buffer[DGLAB_COYOTE_V3_BF_SIZE + 4];
    DglabCoyoteV3Bf params = { 0 };

    memset(bf_buffer, 0xAA, sizeof(bf_buffer));
    dglabCoyoteV3EncodeBf(&params, bf_buffer);

    for (size_t i = DGLAB_COYOTE_V3_BF_SIZE; i < sizeof(bf_buffer); i++)
        CHECK_U8(bf_buffer[i], 0xAA);
}

static void testFrequencyCompression(void)
{
    // Values from the conversion algorithm in coyote/v3/README.md and from the
    // worked examples in coyote/README.md.
    static const struct {
        uint16_t input_ms;
        uint8_t expected;
    } kCases[] = {
        { 10, 10 }, { 20, 20 }, { 50, 50 }, { 100, 100 },
        { 110, 102 }, { 120, 104 }, { 150, 110 },
        { 200, 120 }, { 250, 130 }, { 333, 146 }, { 500, 180 },
        { 680, 208 }, { 750, 215 }, { 1000, 240 },
        // Boundaries between the three branches.
        { 101, 100 }, { 600, 200 }, { 601, 200 }, { 610, 201 },
        // Out of range input falls back to 10, per "else -> 10".
        { 0, 10 }, { 9, 10 }, { 1001, 10 },
    };

    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        g_checks++;
        uint8_t actual = dglabCoyoteV3CompressFrequency(kCases[i].input_ms);
        if (actual != kCases[i].expected) {
            printf("FAIL compressFrequency(%u) = %u, expected %u\n",
                (unsigned int)kCases[i].input_ms, (unsigned int)actual,
                (unsigned int)kCases[i].expected);
            g_failures++;
        }
    }

    // Documented conflict: coyote/README.md lists 650ms -> 204 while the
    // algorithm in coyote/v3/README.md yields 205. The algorithm is the
    // normative source for what to send, so it is what this layer implements.
    CHECK_U8(dglabCoyoteV3CompressFrequency(650), 205);
}

static void testWaveformValidity(void)
{
    DglabCoyoteV3WaveformSlot slot = { 10, 0 };

    CHECK(dglabCoyoteV3IsWaveformSlotValid(&slot));

    slot.frequency = 240;
    slot.strength = 100;
    CHECK(dglabCoyoteV3IsWaveformSlotValid(&slot));

    slot.frequency = 0;
    CHECK(!dglabCoyoteV3IsWaveformSlotValid(&slot));

    slot.frequency = 9;
    CHECK(!dglabCoyoteV3IsWaveformSlotValid(&slot));

    slot.frequency = 241;
    CHECK(!dglabCoyoteV3IsWaveformSlotValid(&slot));

    slot.frequency = 10;
    slot.strength = 101;
    CHECK(!dglabCoyoteV3IsWaveformSlotValid(&slot));

    DglabCoyoteV3WaveformSlot slots[DGLAB_COYOTE_V3_WAVEFORM_SLOTS] = {
        { 10, 0 }, { 20, 50 }, { 30, 100 }, { 40, 10 },
    };
    CHECK(dglabCoyoteV3IsChannelWaveformValid(slots));

    // A single out of range slot invalidates the whole channel, which is the
    // documented "discard all four groups" behaviour.
    slots[2].strength = 101;
    CHECK(!dglabCoyoteV3IsChannelWaveformValid(slots));
}

static void testSetChannelIdle(void)
{
    DglabCoyoteV3B0 packet = {
        .waveform_a = { { 10, 0 }, { 10, 10 }, { 10, 20 }, { 10, 30 } },
        .waveform_b = { { 20, 40 }, { 30, 50 }, { 40, 60 }, { 50, 70 } },
    };
    const DglabCoyoteV3WaveformSlot expected_b[DGLAB_COYOTE_V3_WAVEFORM_SLOTS] = {
        { 20, 40 }, { 30, 50 }, { 40, 60 }, { 50, 70 },
    };

    dglabCoyoteV3SetChannelIdle(&packet, DglabCoyoteV3ChannelA);

    // Zero frequency is outside (10..240), so the device discards channel A
    // while channel B keeps playing.
    CHECK(!dglabCoyoteV3IsChannelWaveformValid(packet.waveform_a));
    CHECK(dglabCoyoteV3IsChannelWaveformValid(packet.waveform_b));
    expectWaveformEquals("idle A keeps B", packet.waveform_b, expected_b);
}

static void testBfRoundTrip(void)
{
    const DglabCoyoteV3Bf params = {
        .soft_limit_a = 150,
        .soft_limit_b = 30,
        .frequency_balance_a = 128,
        .frequency_balance_b = 200,
        .strength_balance_a = 60,
        .strength_balance_b = 255,
    };
    uint8_t encoded[DGLAB_COYOTE_V3_BF_SIZE];
    DglabCoyoteV3Bf decoded = { 0 };

    dglabCoyoteV3EncodeBf(&params, encoded);
    expectBytes("BF encode", encoded, "BF961E80C83CFF", sizeof(encoded));

    CHECK(dglabCoyoteV3DecodeBf(encoded, sizeof(encoded), &decoded));
    CHECK_U8(decoded.soft_limit_a, params.soft_limit_a);
    CHECK_U8(decoded.soft_limit_b, params.soft_limit_b);
    CHECK_U8(decoded.frequency_balance_a, params.frequency_balance_a);
    CHECK_U8(decoded.frequency_balance_b, params.frequency_balance_b);
    CHECK_U8(decoded.strength_balance_a, params.strength_balance_a);
    CHECK_U8(decoded.strength_balance_b, params.strength_balance_b);
}

static void testB1Decode(void)
{
    uint8_t notification[DGLAB_COYOTE_V3_B1_SIZE] = { 0xB1, 0x01, 0x19, 0x05 };
    DglabCoyoteV3B1 decoded = { 0 };

    CHECK(dglabCoyoteV3DecodeB1(notification, sizeof(notification), &decoded));
    CHECK_U8(decoded.sequence, 1);
    CHECK_U8(decoded.strength_a, 25);
    CHECK_U8(decoded.strength_b, 5);

    notification[0] = 0xB0;
    CHECK(!dglabCoyoteV3DecodeB1(notification, sizeof(notification), &decoded));

    notification[0] = 0xB1;
    CHECK(!dglabCoyoteV3DecodeB1(notification, DGLAB_COYOTE_V3_B1_SIZE - 1, &decoded));
    CHECK(!dglabCoyoteV3DecodeB1(NULL, sizeof(notification), &decoded));
    CHECK(!dglabCoyoteV3DecodeB1(notification, sizeof(notification), NULL));

    // A notification carrying trailing bytes is still a valid B1.
    uint8_t padded[DGLAB_COYOTE_V3_B1_SIZE + 1] = { 0xB1, 0x02, 0x0A, 0x0B, 0xFF };
    CHECK(dglabCoyoteV3DecodeB1(padded, sizeof(padded), &decoded));
    CHECK_U8(decoded.sequence, 2);
    CHECK_U8(decoded.strength_a, 10);
    CHECK_U8(decoded.strength_b, 11);
}

static void testDecodeRejectsMalformedPackets(void)
{
    uint8_t packet[DGLAB_COYOTE_V3_B0_SIZE] = { 0 };
    DglabCoyoteV3B0 decoded_b0 = { 0 };
    DglabCoyoteV3Bf decoded_bf = { 0 };
    uint8_t bf[DGLAB_COYOTE_V3_BF_SIZE] = { 0 };

    packet[0] = 0xB0;
    bf[0] = 0xBF;

    CHECK(dglabCoyoteV3DecodeB0(packet, sizeof(packet), &decoded_b0));
    CHECK(!dglabCoyoteV3DecodeB0(packet, DGLAB_COYOTE_V3_B0_SIZE - 1, &decoded_b0));
    CHECK(!dglabCoyoteV3DecodeB0(packet, DGLAB_COYOTE_V3_B0_SIZE + 1, &decoded_b0));
    CHECK(!dglabCoyoteV3DecodeB0(NULL, sizeof(packet), &decoded_b0));
    CHECK(!dglabCoyoteV3DecodeB0(packet, sizeof(packet), NULL));

    packet[0] = 0xBF;
    CHECK(!dglabCoyoteV3DecodeB0(packet, sizeof(packet), &decoded_b0));

    CHECK(dglabCoyoteV3DecodeBf(bf, sizeof(bf), &decoded_bf));
    CHECK(!dglabCoyoteV3DecodeBf(bf, DGLAB_COYOTE_V3_BF_SIZE - 1, &decoded_bf));
    CHECK(!dglabCoyoteV3DecodeBf(bf, DGLAB_COYOTE_V3_BF_SIZE + 1, &decoded_bf));
    CHECK(!dglabCoyoteV3DecodeBf(NULL, sizeof(bf), &decoded_bf));
    CHECK(!dglabCoyoteV3DecodeBf(bf, sizeof(bf), NULL));

    bf[0] = 0xB0;
    CHECK(!dglabCoyoteV3DecodeBf(bf, sizeof(bf), &decoded_bf));
}

static void testGattConstants(void)
{
    // Presence check: the transport layer must use these 16-bit UUIDs rather
    // than re-deriving them, and the device name is used for scanning.
    CHECK_U8(DGLAB_COYOTE_V3_UUID16_SERVICE, 0x180C);
    CHECK_U8(DGLAB_COYOTE_V3_UUID16_CHAR_WRITE, 0x150A);
    CHECK_U8(DGLAB_COYOTE_V3_UUID16_CHAR_NOTIFY, 0x150B);
    CHECK_U8(DGLAB_COYOTE_V3_UUID16_BATTERY_SERVICE, 0x180A);
    CHECK_U8(DGLAB_COYOTE_V3_UUID16_CHAR_BATTERY, 0x1500);
    CHECK(strcmp(DGLAB_COYOTE_V3_DEVICE_NAME, "47L121000") == 0);
}

int main(void)
{
    testOfficialB0Vectors();
    testStrengthModePacking();
    testEncodeDoesNotOverrun();
    testFrequencyCompression();
    testWaveformValidity();
    testSetChannelIdle();
    testBfRoundTrip();
    testB1Decode();
    testDecodeRejectsMalformedPackets();
    testGattConstants();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
