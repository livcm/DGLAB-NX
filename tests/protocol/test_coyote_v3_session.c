// Host side tests for the Coyote V3 session layer: BF on connect, the 100ms B0
// cadence, strength gating and waveform playback.
//
// The session has no BLE dependency, so the transport is replaced by a recorder
// that keeps every packet the session would have written to the device.

#include <dglab/protocol/coyote_v3_session.h>

#include "test_support.h"

#define RECORDER_MAX_WRITES 32
#define RECORDER_MAX_SIZE 32

typedef struct {
    uint8_t data[RECORDER_MAX_WRITES][RECORDER_MAX_SIZE];
    size_t size[RECORDER_MAX_WRITES];
    size_t count;
} Recorder;

static void recorderWrite(void* context, const uint8_t* data, size_t size)
{
    Recorder* recorder = (Recorder*)context;

    if (recorder->count >= RECORDER_MAX_WRITES || size > RECORDER_MAX_SIZE)
        return;

    memcpy(recorder->data[recorder->count], data, size);
    recorder->size[recorder->count] = size;
    recorder->count++;
}

static DglabCoyoteV3SessionConfig defaultConfig(void)
{
    const DglabCoyoteV3SessionConfig config = {
        .bf = {
            .soft_limit_a = 150,
            .soft_limit_b = 30,
            .frequency_balance_a = 128,
            .frequency_balance_b = 200,
            .strength_balance_a = 60,
            .strength_balance_b = 255,
        },
        .strength_feedback = DglabCoyoteV3StrengthFeedback_Required,
    };

    return config;
}

static void makeSession(DglabCoyoteV3Session* session, Recorder* recorder,
    const DglabCoyoteV3SessionConfig* config)
{
    const DglabCoyoteV3Link link = { .write = recorderWrite, .context = recorder };

    dglabCoyoteV3SessionInit(session, &link, config);
}

static DglabCoyoteV3B0 decodeB0Write(const Recorder* recorder, size_t index)
{
    DglabCoyoteV3B0 packet = { 0 };

    g_checks++;
    if (index >= recorder->count) {
        printf("FAIL %s:%d: no write at index %zu\n", __FILE__, __LINE__, index);
        g_failures++;
        return packet;
    }

    CHECK_U8(recorder->size[index], DGLAB_COYOTE_V3_B0_SIZE);
    CHECK(dglabCoyoteV3DecodeB0(recorder->data[index], recorder->size[index], &packet));
    return packet;
}

static void expectWriteCount(const Recorder* recorder, size_t expected)
{
    g_checks++;
    if (recorder->count != expected) {
        printf("FAIL %s:%d: %zu writes, expected %zu\n", __FILE__, __LINE__, recorder->count,
            expected);
        g_failures++;
    }
}

static void testBfIsWrittenOnEveryConnect(void)
{
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    makeSession(&session, &recorder, &config);
    CHECK(!dglabCoyoteV3SessionIsConnected(&session));

    dglabCoyoteV3SessionOnConnected(&session);
    expectWriteCount(&recorder, 1);
    CHECK_U8(recorder.size[0], DGLAB_COYOTE_V3_BF_SIZE);
    expectBytes("connect BF", recorder.data[0], "BF961E80C83CFF", DGLAB_COYOTE_V3_BF_SIZE);

    dglabCoyoteV3SessionOnDisconnected(&session);
    dglabCoyoteV3SessionOnConnected(&session);

    // The device keeps these values across reconnects, but the documentation
    // requires writing them again.
    expectWriteCount(&recorder, 2);
    expectBytes("reconnect BF", recorder.data[1], "BF961E80C83CFF", DGLAB_COYOTE_V3_BF_SIZE);
}

static void testTickCadence(void)
{
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    makeSession(&session, &recorder, &config);
    dglabCoyoteV3SessionOnConnected(&session);
    expectWriteCount(&recorder, 1); // BF only

    dglabCoyoteV3SessionTick(&session, 50);
    expectWriteCount(&recorder, 1);

    dglabCoyoteV3SessionTick(&session, 50);
    expectWriteCount(&recorder, 2);
    CHECK_U8(recorder.size[1], DGLAB_COYOTE_V3_B0_SIZE);

    dglabCoyoteV3SessionTick(&session, 99);
    expectWriteCount(&recorder, 2);

    dglabCoyoteV3SessionTick(&session, 1);
    expectWriteCount(&recorder, 3);

    // A late caller must not turn missed intervals into a burst of writes, and
    // the sub interval remainder is kept so the caller stays in phase.
    dglabCoyoteV3SessionTick(&session, 250);
    expectWriteCount(&recorder, 4);

    dglabCoyoteV3SessionTick(&session, 49);
    expectWriteCount(&recorder, 4);

    dglabCoyoteV3SessionTick(&session, 1);
    expectWriteCount(&recorder, 5);
}

static void testTickAndInputAreIgnoredWhileDisconnected(void)
{
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    makeSession(&session, &recorder, &config);

    dglabCoyoteV3SessionTick(&session, 1000);
    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 5);
    dglabCoyoteV3SessionSetStrengthZero(&session, DglabCoyoteV3ChannelA);
    expectWriteCount(&recorder, 0);

    dglabCoyoteV3SessionOnConnected(&session);
    dglabCoyoteV3SessionTick(&session, 100);
    expectWriteCount(&recorder, 2);

    // Nothing was carried over from before the connection, so the first packet
    // reports "no change".
    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 1);
    CHECK_U8(packet.sequence, 0);
    CHECK(packet.strength_a.mode == DglabCoyoteV3StrengthMode_NoChange);

    dglabCoyoteV3SessionOnDisconnected(&session);
    dglabCoyoteV3SessionTick(&session, 1000);
    expectWriteCount(&recorder, 2);
}

// The canonical packet: strength change plus a channel A waveform whose cycle
// ends inside this packet and is padded.
static void testB0Payload(void)
{
    static const DglabCoyoteV3WaveformEntry waveform_a[] = {
        { 200, 10 },
        { 300, 20 },
        { 400, 30 },
    };
    static const uint8_t expected[] = {
        0xB0, 0x14, 0x05, 0x00, // header, sequence 1 + A increase, A +5, B no change
        0x78, 0x8C, 0xA0, 0xA0, // A 200ms->120, 300ms->140, 400ms->160, padded with 160
        0x0A, 0x14, 0x1E, 0x00, // A strength 10, 20, 30, padded with 0
        0x00, 0x00, 0x00, 0x00, // B idle, discarded by the device
        0x00, 0x00, 0x00, 0x00,
    };
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    makeSession(&session, &recorder, &config);

    CHECK(dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, waveform_a, 3));

    dglabCoyoteV3SessionOnConnected(&session);
    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 5);
    dglabCoyoteV3SessionTick(&session, 100);

    expectWriteCount(&recorder, 2);
    expectByteArray("B0 payload", recorder.data[1], expected, sizeof(expected));

    // The strength change is in flight, so the next packet keeps the waveform
    // but leaves the strength alone.
    dglabCoyoteV3SessionTick(&session, 100);
    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 2);
    CHECK_U8(packet.sequence, 0);
    CHECK(packet.strength_a.mode == DglabCoyoteV3StrengthMode_NoChange);
    CHECK_U8(packet.waveform_a[0].frequency, 120);
    CHECK_U8(packet.waveform_a[3].strength, 0);

    // The B1 for sequence 1 opens the gate again.
    const uint8_t notification[DGLAB_COYOTE_V3_B1_SIZE] = { 0xB1, 0x01, 0x05, 0x00 };
    dglabCoyoteV3SessionOnNotification(&session, notification, sizeof(notification));

    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 2);
    dglabCoyoteV3SessionTick(&session, 100);
    packet = decodeB0Write(&recorder, 3);
    CHECK_U8(packet.sequence, 2);
    CHECK(packet.strength_a.mode == DglabCoyoteV3StrengthMode_RelativeIncrease);
    CHECK_U8(packet.strength_a.value, 2);
}

static void testWaveformCycleIsPacketAligned(void)
{
    static const DglabCoyoteV3WaveformEntry entries[] = {
        { 100, 10 }, { 200, 20 }, { 300, 30 },
        { 400, 40 }, { 500, 50 }, { 600, 60 },
    };
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    makeSession(&session, &recorder, &config);
    CHECK(dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, entries, 6));
    dglabCoyoteV3SessionOnConnected(&session);

    dglabCoyoteV3SessionTick(&session, 100);
    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 1);

    CHECK_U8(packet.waveform_a[0].frequency, 100);
    CHECK_U8(packet.waveform_a[1].frequency, 120);
    CHECK_U8(packet.waveform_a[2].frequency, 140);
    CHECK_U8(packet.waveform_a[3].frequency, 160);
    CHECK_U8(packet.waveform_a[3].strength, 40);

    // The second packet holds the remaining two entries and pads the rest so
    // that the next repetition starts on a packet boundary.
    dglabCoyoteV3SessionTick(&session, 100);
    packet = decodeB0Write(&recorder, 2);

    CHECK_U8(packet.waveform_a[0].frequency, 180);
    CHECK_U8(packet.waveform_a[0].strength, 50);
    CHECK_U8(packet.waveform_a[1].frequency, 200);
    CHECK_U8(packet.waveform_a[1].strength, 60);
    CHECK_U8(packet.waveform_a[2].frequency, 200);
    CHECK_U8(packet.waveform_a[2].strength, 0);
    CHECK_U8(packet.waveform_a[3].frequency, 200);
    CHECK_U8(packet.waveform_a[3].strength, 0);

    dglabCoyoteV3SessionTick(&session, 100);
    packet = decodeB0Write(&recorder, 3);
    CHECK_U8(packet.waveform_a[0].frequency, 100);
    CHECK_U8(packet.waveform_a[0].strength, 10);
}

static void testClearedWaveformGoesIdle(void)
{
    static const DglabCoyoteV3WaveformEntry entries[] = { { 100, 50 } };
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    makeSession(&session, &recorder, &config);
    CHECK(dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, entries, 1));
    dglabCoyoteV3SessionOnConnected(&session);

    dglabCoyoteV3SessionTick(&session, 100);
    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 1);
    CHECK(dglabCoyoteV3IsChannelWaveformValid(packet.waveform_a));

    dglabCoyoteV3SessionClearWaveform(&session, DglabCoyoteV3ChannelA);
    dglabCoyoteV3SessionTick(&session, 100);
    packet = decodeB0Write(&recorder, 2);

    // Zero frequency is out of range, which is the documented way to drive a
    // single channel only.
    CHECK(!dglabCoyoteV3IsChannelWaveformValid(packet.waveform_a));
    CHECK(!dglabCoyoteV3IsChannelWaveformValid(packet.waveform_b));
}

static void testSetWaveformValidation(void)
{
    static const DglabCoyoteV3WaveformEntry valid[] = { { 100, 10 }, { 200, 20 } };
    static const DglabCoyoteV3WaveformEntry bad_frequency[] = { { 5, 10 } };
    static const DglabCoyoteV3WaveformEntry bad_strength[] = { { 100, 101 } };
    DglabCoyoteV3WaveformEntry too_many[DGLAB_COYOTE_V3_WAVEFORM_CAPACITY + 1];
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_CAPACITY + 1; i++)
        too_many[i] = (DglabCoyoteV3WaveformEntry){ 100, 10 };

    makeSession(&session, &recorder, &config);

    CHECK(!dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, NULL, 1));
    CHECK(!dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, valid, 0));
    CHECK(!dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, bad_frequency, 1));
    CHECK(!dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, bad_strength, 1));
    CHECK(!dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, too_many,
        DGLAB_COYOTE_V3_WAVEFORM_CAPACITY + 1));

    // A rejected waveform leaves the previous one untouched.
    CHECK(dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, valid, 2));
    CHECK(!dglabCoyoteV3SessionSetWaveform(&session, DglabCoyoteV3ChannelA, bad_strength, 1));

    dglabCoyoteV3SessionOnConnected(&session);
    dglabCoyoteV3SessionTick(&session, 100);

    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 1);
    CHECK_U8(packet.waveform_a[0].frequency, 100);
    CHECK_U8(packet.waveform_a[0].strength, 10);

    // The last accepted entry is padded with strength 0 for the rest of the
    // packet.
    CHECK_U8(packet.waveform_a[1].frequency, 120);
    CHECK_U8(packet.waveform_a[1].strength, 20);
    CHECK_U8(packet.waveform_a[2].strength, 0);
    CHECK_U8(packet.waveform_a[3].strength, 0);
}

static void testNotificationsOtherThanB1AreIgnored(void)
{
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();
    const uint8_t short_notification[3] = { 0xB1, 0x01, 0x05 };
    uint8_t b0_like[DGLAB_COYOTE_V3_B0_SIZE] = { 0 };

    b0_like[0] = 0xB0;

    makeSession(&session, &recorder, &config);
    dglabCoyoteV3SessionOnConnected(&session);
    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 5);
    dglabCoyoteV3SessionTick(&session, 100);

    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 1);
    CHECK_U8(packet.sequence, 1);

    dglabCoyoteV3SessionOnNotification(&session, b0_like, sizeof(b0_like));
    dglabCoyoteV3SessionOnNotification(&session, short_notification, sizeof(short_notification));
    dglabCoyoteV3SessionOnNotification(&session, NULL, 0);

    // Still waiting for sequence 1.
    dglabCoyoteV3SessionTick(&session, 100);
    packet = decodeB0Write(&recorder, 2);
    CHECK_U8(packet.sequence, 0);

    const uint8_t notification[DGLAB_COYOTE_V3_B1_SIZE] = { 0xB1, 0x01, 0x05, 0x00 };
    dglabCoyoteV3SessionOnNotification(&session, notification, sizeof(notification));

    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 1);
    dglabCoyoteV3SessionTick(&session, 100);
    packet = decodeB0Write(&recorder, 3);
    CHECK_U8(packet.sequence, 2);
}

static void testDisconnectDropsInFlightStrength(void)
{
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    const DglabCoyoteV3SessionConfig config = defaultConfig();

    makeSession(&session, &recorder, &config);
    dglabCoyoteV3SessionOnConnected(&session);
    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 5);
    dglabCoyoteV3SessionTick(&session, 100);
    CHECK_U8(decodeB0Write(&recorder, 1).sequence, 1);

    dglabCoyoteV3SessionOnDisconnected(&session);
    dglabCoyoteV3SessionOnConnected(&session);

    // The in flight change and its B1 are gone with the connection.
    dglabCoyoteV3SessionTick(&session, 100);
    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 3);
    CHECK_U8(packet.sequence, 0);
    CHECK(packet.strength_a.mode == DglabCoyoteV3StrengthMode_NoChange);
}

static void testFireAndForgetSession(void)
{
    Recorder recorder = { 0 };
    DglabCoyoteV3Session session;
    DglabCoyoteV3SessionConfig config = defaultConfig();

    config.strength_feedback = DglabCoyoteV3StrengthFeedback_None;

    makeSession(&session, &recorder, &config);
    dglabCoyoteV3SessionOnConnected(&session);
    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 10);
    dglabCoyoteV3SessionTick(&session, 100);

    DglabCoyoteV3B0 packet = decodeB0Write(&recorder, 1);
    CHECK_U8(packet.sequence, 0);
    CHECK(packet.strength_a.mode == DglabCoyoteV3StrengthMode_RelativeIncrease);
    CHECK_U8(packet.strength_a.value, 10);

    // No B1 is needed to send the next change.
    dglabCoyoteV3SessionAdjustStrength(&session, DglabCoyoteV3ChannelA, 10);
    dglabCoyoteV3SessionTick(&session, 100);

    packet = decodeB0Write(&recorder, 2);
    CHECK_U8(packet.sequence, 0);
    CHECK_U8(packet.strength_a.value, 10);
}

int main(void)
{
    testBfIsWrittenOnEveryConnect();
    testTickCadence();
    testTickAndInputAreIgnoredWhileDisconnected();
    testB0Payload();
    testWaveformCycleIsPacketAligned();
    testClearedWaveformGoesIdle();
    testSetWaveformValidation();
    testNotificationsOtherThanB1AreIgnored();
    testDisconnectDropsInFlightStrength();
    testFireAndForgetSession();

    return testFinish();
}
