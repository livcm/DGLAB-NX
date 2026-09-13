// Host side tests for the Coyote V3 strength / sequence state machine.
//
// The scenarios follow the walkthrough published in coyote/v3/README.md:
// accumulate user input, send it once per B0 packet, and (in the default
// feedback mode) wait for the matching B1 before sending another change.

#include <dglab/protocol/coyote_v3_strength.h>

#include "test_support.h"

static DglabCoyoteV3StrengthState makeState(DglabCoyoteV3StrengthFeedback feedback)
{
    DglabCoyoteV3StrengthState state;

    dglabCoyoteV3StrengthStateInit(&state, feedback);
    return state;
}

static DglabCoyoteV3B0 takePacket(DglabCoyoteV3StrengthState* state)
{
    DglabCoyoteV3B0 packet = { 0 };

    dglabCoyoteV3StrengthStateApplyToPacket(state, &packet);
    return packet;
}

static void expectStrength(const char* name, const DglabCoyoteV3Strength* strength,
    DglabCoyoteV3StrengthMode mode, uint8_t value)
{
    g_checks++;
    if (strength->mode != mode || strength->value != value) {
        printf("FAIL %s: got mode %d value %u, expected mode %d value %u\n", name,
            (int)strength->mode, (unsigned int)strength->value, (int)mode,
            (unsigned int)value);
        g_failures++;
    }
}

static void confirm(DglabCoyoteV3StrengthState* state, uint8_t sequence, uint8_t strength_a,
    uint8_t strength_b)
{
    const DglabCoyoteV3B1 notification = {
        .sequence = sequence,
        .strength_a = strength_a,
        .strength_b = strength_b,
    };

    dglabCoyoteV3StrengthStateOnB1(state, &notification);
}

static void testIdlePacket(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);
    DglabCoyoteV3B0 packet = takePacket(&state);

    CHECK_U8(packet.sequence, 0);
    expectStrength("idle A", &packet.strength_a, DglabCoyoteV3StrengthMode_NoChange, 0);
    expectStrength("idle B", &packet.strength_b, DglabCoyoteV3StrengthMode_NoChange, 0);
    CHECK(!dglabCoyoteV3StrengthStateIsWaiting(&state));
    CHECK_U8(dglabCoyoteV3StrengthStateDeviceStrength(&state, DglabCoyoteV3ChannelA), 0);
}

// Mirrors the documented timeline: send the first change with a sequence
// number, keep accumulating while the B1 is outstanding, then send the
// accumulated value once the device confirms.
static void testGatedSequence(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);

    // "1 -> 按下 A 通道强度 + 按钮" / "2 -> B0 准备写入"
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 1);

    DglabCoyoteV3B0 packet = takePacket(&state);

    CHECK_U8(packet.sequence, 1);
    expectStrength("first change", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeIncrease, 1);
    expectStrength("first change B", &packet.strength_b, DglabCoyoteV3StrengthMode_NoChange, 0);
    CHECK(dglabCoyoteV3StrengthStateIsWaiting(&state));

    // "3 -> (100ms 周期) B0 准备写入": the gate is closed, but the user keeps
    // pressing, so the delta has to accumulate rather than be dropped.
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 3);

    packet = takePacket(&state);

    CHECK_U8(packet.sequence, 0);
    expectStrength("blocked", &packet.strength_a, DglabCoyoteV3StrengthMode_NoChange, 0);
    expectStrength("blocked B", &packet.strength_b, DglabCoyoteV3StrengthMode_NoChange, 0);
    CHECK(dglabCoyoteV3StrengthStateIsWaiting(&state));

    // "150B 返回 A 通道强度值" for the first change.
    confirm(&state, 1, 1, 0);

    CHECK(!dglabCoyoteV3StrengthStateIsWaiting(&state));
    CHECK_U8(dglabCoyoteV3StrengthStateDeviceStrength(&state, DglabCoyoteV3ChannelA), 1);

    packet = takePacket(&state);

    CHECK_U8(packet.sequence, 2);
    expectStrength("accumulated", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeIncrease, 3);
}

static void testRelativeDecreaseAndMixedPacket(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);

    // Both channels are packed into one B0 packet, so they share one sequence
    // number and one gate.
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 2);
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelB, -3);

    DglabCoyoteV3B0 packet = takePacket(&state);

    CHECK_U8(packet.sequence, 1);
    expectStrength("mixed A", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeIncrease, 2);
    expectStrength("mixed B", &packet.strength_b, DglabCoyoteV3StrengthMode_RelativeDecrease, 3);

    confirm(&state, 1, 12, 7);
    CHECK_U8(dglabCoyoteV3StrengthStateDeviceStrength(&state, DglabCoyoteV3ChannelA), 12);
    CHECK_U8(dglabCoyoteV3StrengthStateDeviceStrength(&state, DglabCoyoteV3ChannelB), 7);

    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, -4);

    packet = takePacket(&state);

    CHECK_U8(packet.sequence, 2);
    expectStrength("decrease", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeDecrease, 4);
}

static void testUnrelatedB1DoesNotOpenTheGate(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);

    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 5);
    DglabCoyoteV3B0 packet = takePacket(&state);
    CHECK_U8(packet.sequence, 1);

    // A wheel turn reports sequence 0 while a change is in flight.
    confirm(&state, 0, 20, 0);
    CHECK(dglabCoyoteV3StrengthStateIsWaiting(&state));
    CHECK_U8(dglabCoyoteV3StrengthStateDeviceStrength(&state, DglabCoyoteV3ChannelA), 20);

    // A stale sequence number must not open the gate either.
    confirm(&state, 3, 21, 0);
    CHECK(dglabCoyoteV3StrengthStateIsWaiting(&state));
    CHECK_U8(dglabCoyoteV3StrengthStateDeviceStrength(&state, DglabCoyoteV3ChannelA), 21);

    confirm(&state, 1, 22, 0);
    CHECK(!dglabCoyoteV3StrengthStateIsWaiting(&state));
}

static void testAbsoluteZero(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);

    // strengthZero() must win over whatever the user queued.
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 7);
    dglabCoyoteV3StrengthStateRequestZero(&state, DglabCoyoteV3ChannelA);

    DglabCoyoteV3B0 packet = takePacket(&state);

    CHECK_U8(packet.sequence, 1);
    expectStrength("zero A", &packet.strength_a, DglabCoyoteV3StrengthMode_Absolute, 0);
    expectStrength("zero B", &packet.strength_b, DglabCoyoteV3StrengthMode_NoChange, 0);

    confirm(&state, 1, 0, 0);

    // The queued +7 was replaced by the absolute zero, so nothing is left.
    packet = takePacket(&state);
    CHECK_U8(packet.sequence, 0);
    expectStrength("zero drained", &packet.strength_a, DglabCoyoteV3StrengthMode_NoChange, 0);
}

static void testZeroWaitsForTheGate(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);

    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 1);
    takePacket(&state);

    // A zero request while a change is in flight has to wait like any other
    // strength change; it is not lost.
    dglabCoyoteV3StrengthStateRequestZero(&state, DglabCoyoteV3ChannelA);

    DglabCoyoteV3B0 packet = takePacket(&state);
    expectStrength("zero blocked", &packet.strength_a, DglabCoyoteV3StrengthMode_NoChange, 0);

    confirm(&state, 1, 1, 0);

    packet = takePacket(&state);
    CHECK_U8(packet.sequence, 2);
    expectStrength("zero released", &packet.strength_a, DglabCoyoteV3StrengthMode_Absolute, 0);
}

static void testSequenceWrapsAfter15(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);

    for (uint8_t sequence = DGLAB_COYOTE_V3_SEQUENCE_MIN;
         sequence <= DGLAB_COYOTE_V3_SEQUENCE_MAX; sequence++) {
        dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 1);

        DglabCoyoteV3B0 packet = takePacket(&state);
        CHECK_U8(packet.sequence, sequence);

        confirm(&state, sequence, sequence, 0);
    }

    // 0 is reserved for "no feedback", so the counter restarts at 1.
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 1);
    DglabCoyoteV3B0 packet = takePacket(&state);
    CHECK_U8(packet.sequence, 1);
}

static void testAccumulatedValueIsClamped(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_Required);

    // 300 would not fit in the byte field of the packet; the device cannot
    // resolve more than 200 either, so the value is clamped instead of wrapping.
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 300);

    DglabCoyoteV3B0 packet = takePacket(&state);
    expectStrength("clamped up", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeIncrease, 200);

    confirm(&state, 1, 200, 0);

    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, -1000);
    packet = takePacket(&state);
    expectStrength("clamped down", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeDecrease, 200);
}

// The documentation also allows writing strength changes with sequence 0 when
// the app does not need the device to confirm them.
static void testFireAndForgetMode(void)
{
    DglabCoyoteV3StrengthState state = makeState(DglabCoyoteV3StrengthFeedback_None);

    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 5);

    DglabCoyoteV3B0 packet = takePacket(&state);
    CHECK_U8(packet.sequence, 0);
    expectStrength("unconfirmed", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeIncrease, 5);
    CHECK(!dglabCoyoteV3StrengthStateIsWaiting(&state));

    // Nothing gates the next change, so back to back packets are allowed.
    dglabCoyoteV3StrengthStateAdjust(&state, DglabCoyoteV3ChannelA, 5);
    packet = takePacket(&state);
    CHECK_U8(packet.sequence, 0);
    expectStrength("unconfirmed again", &packet.strength_a, DglabCoyoteV3StrengthMode_RelativeIncrease, 5);

    confirm(&state, 0, 10, 0);
    CHECK_U8(dglabCoyoteV3StrengthStateDeviceStrength(&state, DglabCoyoteV3ChannelA), 10);

    dglabCoyoteV3StrengthStateRequestZero(&state, DglabCoyoteV3ChannelA);
    packet = takePacket(&state);
    CHECK_U8(packet.sequence, 0);
    expectStrength("unconfirmed zero", &packet.strength_a, DglabCoyoteV3StrengthMode_Absolute, 0);
}

int main(void)
{
    testIdlePacket();
    testGatedSequence();
    testRelativeDecreaseAndMixedPacket();
    testUnrelatedB1DoesNotOpenTheGate();
    testAbsoluteZero();
    testZeroWaitsForTheGate();
    testSequenceWrapsAfter15();
    testAccumulatedValueIsClamped();
    testFireAndForgetMode();

    return testFinish();
}
