#pragma once

// DG-LAB Coyote V3 Bluetooth Protocol.
//
// Source of truth:
//   https://github.com/dungeonlab-open/dglab-bluetooth-protocol
//   coyote/v3/README.md and coyote/README.md
//   verified against commit 0155a23cfc123d1243b96482ea475d147573ce8f (2026-06-11)
//
// This protocol layer is deliberately platform independent: it only uses the C
// standard headers below and never includes libnx or HOS types. That keeps the
// packet encoding testable on a PC (see tests/protocol) and leaves the BLE
// transport as the only Switch specific part.
//
// Byte order: unlike V2, the V3 protocol performs no endianness conversion. All
// fields below are single bytes, so no byte swapping is needed anywhere.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// GATT attribute UUIDs, 16-bit form. The base UUID is
// 0000xxxx-0000-1000-8000-00805f9b34fb.
#define DGLAB_COYOTE_V3_UUID16_SERVICE         0x180Cu
#define DGLAB_COYOTE_V3_UUID16_CHAR_WRITE      0x150Au
#define DGLAB_COYOTE_V3_UUID16_CHAR_NOTIFY     0x150Bu
#define DGLAB_COYOTE_V3_UUID16_BATTERY_SERVICE 0x180Au
#define DGLAB_COYOTE_V3_UUID16_CHAR_BATTERY    0x1500u

// Advertised Bluetooth name of the Coyote 3.0 pulse host.
#define DGLAB_COYOTE_V3_DEVICE_NAME "47L121000"

// Packet sizes.
#define DGLAB_COYOTE_V3_B0_SIZE 20u
#define DGLAB_COYOTE_V3_BF_SIZE 7u
#define DGLAB_COYOTE_V3_B1_SIZE 4u

// Each B0 packet carries this many waveform slots per channel. One slot covers
// 25ms of output, so one packet covers 100ms.
#define DGLAB_COYOTE_V3_WAVEFORM_SLOTS 4u

// Value ranges defined by the official V3 documentation.
#define DGLAB_COYOTE_V3_STRENGTH_MAX           200u
#define DGLAB_COYOTE_V3_WAVEFORM_FREQ_MIN       10u
#define DGLAB_COYOTE_V3_WAVEFORM_FREQ_MAX      240u
#define DGLAB_COYOTE_V3_WAVEFORM_STRENGTH_MAX  100u

// Command headers.
#define DGLAB_COYOTE_V3_HEADER_B0 0xB0u
#define DGLAB_COYOTE_V3_HEADER_BF 0xBFu
#define DGLAB_COYOTE_V3_HEADER_B1 0xB1u

typedef enum {
    DglabCoyoteV3ChannelA = 0,
    DglabCoyoteV3ChannelB = 1,
} DglabCoyoteV3Channel;

// How the device interprets DglabCoyoteV3Strength::value.
//
// The two bits are packed per channel into the low nibble of the B0 sequence
// byte: bits 3..2 for channel A, bits 1..0 for channel B.
typedef enum {
    DglabCoyoteV3StrengthMode_NoChange         = 0x0,
    DglabCoyoteV3StrengthMode_RelativeIncrease = 0x1,
    DglabCoyoteV3StrengthMode_RelativeDecrease = 0x2,
    DglabCoyoteV3StrengthMode_Absolute         = 0x3,
} DglabCoyoteV3StrengthMode;

// One strength change request for a single channel.
//
// value is the strength setting (0..200). The device treats values outside
// (0..200) as 0, so this layer does not clamp: it encodes what the caller
// asked for and leaves that documented rule to the device.
//
// The device clamps the result of absolute mode to (0..200) and applies the
// channel soft limit configured through the BF command.
typedef struct {
    DglabCoyoteV3StrengthMode mode;
    uint8_t value;
} DglabCoyoteV3Strength;

// One 25ms waveform output slot.
//
// frequency is the *device* value (10..240), not the 10..1000 range the app
// exposes: run the app side value through dglabCoyoteV3CompressFrequency()
// first. strength is 0..100.
typedef struct {
    uint8_t frequency;
    uint8_t strength;
} DglabCoyoteV3WaveformSlot;

// B0 command: channel strengths plus waveform data for both channels.
typedef struct {
    // Sequence number, 4 bits (0..15). Set 0 to let the device apply a strength
    // change silently; any other value makes the device answer with a B1
    // notification carrying the same sequence number.
    uint8_t sequence;
    DglabCoyoteV3Strength strength_a;
    DglabCoyoteV3Strength strength_b;
    DglabCoyoteV3WaveformSlot waveform_a[DGLAB_COYOTE_V3_WAVEFORM_SLOTS];
    DglabCoyoteV3WaveformSlot waveform_b[DGLAB_COYOTE_V3_WAVEFORM_SLOTS];
} DglabCoyoteV3B0;

// BF command: channel soft limits and waveform balance parameters.
//
// The device ignores out-of-range soft limits instead of treating them as 0.
// Both soft limits and balance parameters are persisted by the device, but the
// official documentation still requires writing BF again after every
// reconnect so that the limits are known rather than inherited.
typedef struct {
    uint8_t soft_limit_a;        // 0..200, out of range: keep current value
    uint8_t soft_limit_b;        // 0..200, out of range: keep current value
    uint8_t frequency_balance_a; // 0..255
    uint8_t frequency_balance_b; // 0..255
    uint8_t strength_balance_a;  // 0..255
    uint8_t strength_balance_b;  // 0..255
} DglabCoyoteV3Bf;

// B1 notification: current actual channel strengths.
//
// The device sends this whenever a strength changes. sequence is 0 unless the
// change was caused by a B0 command that used a non-zero sequence number.
typedef struct {
    uint8_t sequence;
    uint8_t strength_a;
    uint8_t strength_b;
} DglabCoyoteV3B1;

// Converts an app side waveform frequency in milliseconds (10..1000) to the
// value the device expects (10..240). Values outside (10..1000) return 10, as
// specified by the official conversion algorithm.
uint8_t dglabCoyoteV3CompressFrequency(uint16_t frequency_ms);

// Range checks matching the documented device behaviour.
bool dglabCoyoteV3IsWaveformSlotValid(const DglabCoyoteV3WaveformSlot* slot);

// A channel's waveform data is accepted only when all four slots are valid.
// If any slot is out of range the device discards the channel's whole packet,
// which is also the documented way to drive a single channel only.
bool dglabCoyoteV3IsChannelWaveformValid(const DglabCoyoteV3WaveformSlot* slots);

// Marks a channel as idle by zeroing its four waveform slots. Zero frequency is
// outside (10..240), so the device discards the channel's data while the other
// channel keeps playing (the official documentation uses the same technique
// with an out-of-range strength value).
void dglabCoyoteV3SetChannelIdle(DglabCoyoteV3B0* packet, DglabCoyoteV3Channel channel);

// Encodes to exactly DGLAB_COYOTE_V3_B0_SIZE bytes. out must have room for it.
void dglabCoyoteV3EncodeB0(const DglabCoyoteV3B0* packet, uint8_t* out);

// Decodes DGLAB_COYOTE_V3_B0_SIZE bytes. Returns false on a wrong header or
// size. Values are copied verbatim, including out of range ones.
bool dglabCoyoteV3DecodeB0(const uint8_t* in, size_t size, DglabCoyoteV3B0* out);

// Encodes to exactly DGLAB_COYOTE_V3_BF_SIZE bytes. out must have room for it.
void dglabCoyoteV3EncodeBf(const DglabCoyoteV3Bf* params, uint8_t* out);

// Decodes DGLAB_COYOTE_V3_BF_SIZE bytes. Returns false on a wrong header or
// size.
bool dglabCoyoteV3DecodeBf(const uint8_t* in, size_t size, DglabCoyoteV3Bf* out);

// Decodes a B1 notification of at least DGLAB_COYOTE_V3_B1_SIZE bytes. Returns
// false on a wrong header or size. A notification with trailing bytes is
// accepted; only the documented fields are read.
bool dglabCoyoteV3DecodeB1(const uint8_t* in, size_t size, DglabCoyoteV3B1* out);
