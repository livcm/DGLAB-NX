#pragma once

// Coyote V3 channel strength bookkeeping.
//
// The official V3 documentation describes how an app should sequence strength
// changes: accumulate the user's + / - input, send it once per B0 packet with a
// non-zero sequence number, and wait for the matching B1 notification before
// sending another strength change. This file implements that reference
// behaviour as a pure state machine so it can be tested without a device.
//
// Source of truth:
//   https://github.com/dungeonlab-open/dglab-bluetooth-protocol
//   coyote/v3/README.md (strengthDataProcessingA / strengthDataCallback /
//   strengthZero) @ 0155a23cfc123d1243b96482ea475d147573ce8f
//
// Deviation from the reference pseudo code, on purpose:
//   - The reference shows one orderNo per channel, but B0 carries a single
//     sequence number for both channels. This implementation uses one shared
//     counter and one shared "waiting for B1" gate for the whole packet.
//   - The sequence number is 4 bits and 0 means "no feedback", so the counter
//     runs 1..15 and wraps back to 1.

#include <dglab/protocol/coyote_v3.h>

#include <stdint.h>

// Sequence numbers are 4 bits wide; 0 is reserved for "the device should not
// answer".
#define DGLAB_COYOTE_V3_SEQUENCE_MIN 1u
#define DGLAB_COYOTE_V3_SEQUENCE_MAX 15u

// How strength changes are written. The official documentation offers both:
// sequence 0 means "the device does not have to answer", while a non-zero
// sequence number makes the device confirm through B1. The reference algorithm
// in the documentation uses the confirming variant.
//
// The confirming variant is deliberately the zero value, so a zero initialized
// configuration selects the safer behaviour instead of silently opting out of
// the confirmation.
typedef enum {
    // Write with a non-zero sequence number and wait for the matching B1
    // before sending the next strength change.
    DglabCoyoteV3StrengthFeedback_Required = 0,
    // Always write with sequence number 0 and never wait for a B1.
    DglabCoyoteV3StrengthFeedback_None = 1,
} DglabCoyoteV3StrengthFeedback;

typedef struct {
    DglabCoyoteV3StrengthFeedback feedback;
    // Strength changes accumulated since the last B0 packet was built.
    int32_t pending_a;
    int32_t pending_b;
    // Absolute zero requests that have not been sent yet. A zero request wins
    // over the pending deltas of the same channel, which are dropped.
    bool zero_pending_a;
    bool zero_pending_b;
    // True while a strength change is in flight and waiting for its B1.
    bool waiting_for_b1;
    // Last sequence number handed out (1..15).
    uint8_t sequence;
    // Sequence number of the in flight change, 0 when nothing is in flight.
    uint8_t inflight_sequence;
    // Channel strengths last reported by the device. They stay 0 until the
    // device reports something.
    uint8_t device_strength_a;
    uint8_t device_strength_b;
} DglabCoyoteV3StrengthState;

void dglabCoyoteV3StrengthStateInit(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3StrengthFeedback feedback);

// Adds a relative strength change, in the 0..200 range the device uses.
// Positive increases, negative decreases. Values accumulate while a change is
// waiting for its B1 notification.
void dglabCoyoteV3StrengthStateAdjust(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3Channel channel, int32_t delta);

// Requests an absolute zero on the next allowed packet, mirroring the
// documented strengthZero(). Queued deltas for that channel are dropped.
void dglabCoyoteV3StrengthStateRequestZero(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3Channel channel);

// Fills the strength related fields of the next B0 packet.
//
// With DglabCoyoteV3StrengthFeedback_Required, a change that is still in flight
// makes this write "no change" for both channels with sequence number 0, which
// keeps the waveform playing without touching the strength. Otherwise it
// consumes the pending deltas, or reports "no change" with sequence 0 when
// there is nothing to send.
void dglabCoyoteV3StrengthStateApplyToPacket(DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3B0* packet);

// Feeds a B1 notification back into the state machine. Any B1 updates the known
// device strengths; only a B1 carrying the in flight sequence number releases
// the gate, which matches the documented strengthDataCallback().
void dglabCoyoteV3StrengthStateOnB1(DglabCoyoteV3StrengthState* state,
    const DglabCoyoteV3B1* notification);

// Last strength reported by the device for a channel.
uint8_t dglabCoyoteV3StrengthStateDeviceStrength(const DglabCoyoteV3StrengthState* state,
    DglabCoyoteV3Channel channel);

// True while a strength change is waiting for its B1 notification.
bool dglabCoyoteV3StrengthStateIsWaiting(const DglabCoyoteV3StrengthState* state);
