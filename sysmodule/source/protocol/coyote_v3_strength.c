#include <dglab/protocol/coyote_v3_strength.h>

#include <string.h>

// Keeps accumulated values inside the range the device accepts. Relative
// changes larger than 200 cannot produce a different result, because the device
// clamps the absolute channel strength to 0..200 either way. Clamping here also
// stops a long input burst from overflowing the accumulator.
static int32_t clampToStrengthRange(int32_t value)
{
    if (value > (int32_t)DGLAB_COYOTE_V3_STRENGTH_MAX)
        return (int32_t)DGLAB_COYOTE_V3_STRENGTH_MAX;

    if (value < -(int32_t)DGLAB_COYOTE_V3_STRENGTH_MAX)
        return -(int32_t)DGLAB_COYOTE_V3_STRENGTH_MAX;

    return value;
}

// 1..15, wrapping back to 1. 0 is reserved for "the device should not answer".
static uint8_t nextSequence(uint8_t current)
{
    if (current < DGLAB_COYOTE_V3_SEQUENCE_MIN || current >= DGLAB_COYOTE_V3_SEQUENCE_MAX)
        return (uint8_t)DGLAB_COYOTE_V3_SEQUENCE_MIN;

    return (uint8_t)(current + 1);
}

static int32_t* pendingFor(DglabCoyoteV3StrengthState* state, DglabCoyoteV3Channel channel)
{
    return (channel == DglabCoyoteV3ChannelA) ? &state->pending_a : &state->pending_b;
}

static bool* zeroPendingFor(DglabCoyoteV3StrengthState* state, DglabCoyoteV3Channel channel)
{
    return (channel == DglabCoyoteV3ChannelA) ? &state->zero_pending_a : &state->zero_pending_b;
}

static void applyChannel(DglabCoyoteV3StrengthState* state, DglabCoyoteV3Channel channel,
    bool zero_pending, int32_t pending, DglabCoyoteV3Strength* out, bool* changed)
{
    if (zero_pending) {
        out->mode = DglabCoyoteV3StrengthMode_Absolute;
        out->value = 0;
        *zeroPendingFor(state, channel) = false;
        *changed = true;
        return;
    }

    if (pending == 0)
        return;

    int32_t delta = clampToStrengthRange(pending);

    out->mode = (delta > 0) ? DglabCoyoteV3StrengthMode_RelativeIncrease
                            : DglabCoyoteV3StrengthMode_RelativeDecrease;
    out->value = (uint8_t)((delta > 0) ? delta : -delta);

    *pendingFor(state, channel) = 0;
    *changed = true;
}

void dglabCoyoteV3StrengthStateInit(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3StrengthFeedback feedback)
{
    memset(state, 0, sizeof(*state));
    state->feedback = feedback;
}

void dglabCoyoteV3StrengthStateAdjust(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3Channel channel, int32_t delta)
{
    int32_t* pending = pendingFor(state, channel);
    *pending = clampToStrengthRange(*pending + delta);
}

void dglabCoyoteV3StrengthStateRequestZero(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3Channel channel)
{
    // An absolute zero supersedes whatever the user queued for this channel.
    *pendingFor(state, channel) = 0;
    *zeroPendingFor(state, channel) = true;
}

void dglabCoyoteV3StrengthStateApplyToPacket(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3B0* packet)
{
    packet->sequence = 0;
    packet->strength_a.mode = DglabCoyoteV3StrengthMode_NoChange;
    packet->strength_a.value = 0;
    packet->strength_b.mode = DglabCoyoteV3StrengthMode_NoChange;
    packet->strength_b.value = 0;

    // A change is still in flight: keep the strength untouched until the device
    // confirms it. The waveform in this packet keeps playing.
    if (state->feedback == DglabCoyoteV3StrengthFeedback_Required && state->waiting_for_b1)
        return;

    bool changed = false;

    applyChannel(state, DglabCoyoteV3ChannelA, state->zero_pending_a, state->pending_a,
        &packet->strength_a, &changed);
    applyChannel(state, DglabCoyoteV3ChannelB, state->zero_pending_b, state->pending_b,
        &packet->strength_b, &changed);

    if (!changed)
        return;

    // Without feedback the packet is written with sequence number 0, so the
    // device applies the change silently and there is nothing to wait for.
    if (state->feedback != DglabCoyoteV3StrengthFeedback_Required)
        return;

    state->sequence = nextSequence(state->sequence);
    state->inflight_sequence = state->sequence;
    state->waiting_for_b1 = true;
    packet->sequence = state->sequence;
}

void dglabCoyoteV3StrengthStateOnB1(DglabCoyoteV3StrengthState* state,
    const DglabCoyoteV3B1* notification)
{
    state->device_strength_a = notification->strength_a;
    state->device_strength_b = notification->strength_b;

    uint8_t sequence = (uint8_t)(notification->sequence & 0x0Fu);

    if (sequence == 0 || !state->waiting_for_b1 || sequence != state->inflight_sequence)
        return;

    state->waiting_for_b1 = false;
    state->inflight_sequence = 0;
}

uint8_t dglabCoyoteV3StrengthStateDeviceStrength(const DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3Channel channel)
{
    return (channel == DglabCoyoteV3ChannelA) ? state->device_strength_a
                                              : state->device_strength_b;
}

bool dglabCoyoteV3StrengthStateIsWaiting(const DglabCoyoteV3StrengthState* state)
{
    return state->waiting_for_b1;
}
