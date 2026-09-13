#include <dglab/protocol/coyote_v3.h>

// B0 layout (20 bytes):
//   0      header 0xB0
//   1      sequence (4 bits) | strength mode A (2 bits) | strength mode B (2 bits)
//   2      strength setting A
//   3      strength setting B
//   4..7   waveform frequency A x4
//   8..11  waveform strength A x4
//   12..15 waveform frequency B x4
//   16..19 waveform strength B x4
#define B0_OFFSET_SEQUENCE          1u
#define B0_OFFSET_STRENGTH_A        2u
#define B0_OFFSET_STRENGTH_B        3u
#define B0_OFFSET_WAVEFORM_A_FREQ   4u
#define B0_OFFSET_WAVEFORM_A_STRENGTH 8u
#define B0_OFFSET_WAVEFORM_B_FREQ   12u
#define B0_OFFSET_WAVEFORM_B_STRENGTH 16u

static uint8_t encodeSequenceAndModes(const DglabCoyoteV3B0* packet)
{
    uint8_t sequence = (uint8_t)(packet->sequence & 0x0Fu);
    uint8_t mode_a = (uint8_t)((uint8_t)packet->strength_a.mode & 0x03u);
    uint8_t mode_b = (uint8_t)((uint8_t)packet->strength_b.mode & 0x03u);

    return (uint8_t)((sequence << 4) | (mode_a << 2) | mode_b);
}

static void encodeChannelWaveform(const DglabCoyoteV3WaveformSlot* slots,
    uint8_t* frequencies, uint8_t* strengths)
{
    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++) {
        frequencies[i] = slots[i].frequency;
        strengths[i] = slots[i].strength;
    }
}

static void decodeChannelWaveform(const uint8_t* frequencies, const uint8_t* strengths,
    DglabCoyoteV3WaveformSlot* slots)
{
    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++) {
        slots[i].frequency = frequencies[i];
        slots[i].strength = strengths[i];
    }
}

uint8_t dglabCoyoteV3CompressFrequency(uint16_t frequency_ms)
{
    // Official conversion algorithm:
    //   in 10..100  -> value
    //   in 101..600 -> (value - 100) / 5 + 100
    //   in 601..1000 -> (value - 600) / 10 + 200
    //   otherwise   -> 10
    if (frequency_ms >= 10u && frequency_ms <= 100u)
        return (uint8_t)frequency_ms;

    if (frequency_ms >= 101u && frequency_ms <= 600u)
        return (uint8_t)((frequency_ms - 100u) / 5u + 100u);

    if (frequency_ms >= 601u && frequency_ms <= 1000u)
        return (uint8_t)((frequency_ms - 600u) / 10u + 200u);

    return (uint8_t)DGLAB_COYOTE_V3_WAVEFORM_FREQ_MIN;
}

bool dglabCoyoteV3IsWaveformSlotValid(const DglabCoyoteV3WaveformSlot* slot)
{
    return slot->frequency >= DGLAB_COYOTE_V3_WAVEFORM_FREQ_MIN &&
           slot->frequency <= DGLAB_COYOTE_V3_WAVEFORM_FREQ_MAX &&
           slot->strength <= DGLAB_COYOTE_V3_WAVEFORM_STRENGTH_MAX;
}

bool dglabCoyoteV3IsChannelWaveformValid(const DglabCoyoteV3WaveformSlot* slots)
{
    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++) {
        if (!dglabCoyoteV3IsWaveformSlotValid(&slots[i]))
            return false;
    }

    return true;
}

void dglabCoyoteV3SetChannelIdle(DglabCoyoteV3B0* packet, DglabCoyoteV3Channel channel)
{
    DglabCoyoteV3WaveformSlot* slots =
        (channel == DglabCoyoteV3ChannelA) ? packet->waveform_a : packet->waveform_b;

    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++) {
        slots[i].frequency = 0;
        slots[i].strength = 0;
    }
}

void dglabCoyoteV3EncodeB0(const DglabCoyoteV3B0* packet, uint8_t* out)
{
    out[0] = (uint8_t)DGLAB_COYOTE_V3_HEADER_B0;
    out[B0_OFFSET_SEQUENCE] = encodeSequenceAndModes(packet);
    out[B0_OFFSET_STRENGTH_A] = packet->strength_a.value;
    out[B0_OFFSET_STRENGTH_B] = packet->strength_b.value;

    encodeChannelWaveform(packet->waveform_a,
        out + B0_OFFSET_WAVEFORM_A_FREQ, out + B0_OFFSET_WAVEFORM_A_STRENGTH);
    encodeChannelWaveform(packet->waveform_b,
        out + B0_OFFSET_WAVEFORM_B_FREQ, out + B0_OFFSET_WAVEFORM_B_STRENGTH);
}

bool dglabCoyoteV3DecodeB0(const uint8_t* in, size_t size, DglabCoyoteV3B0* out)
{
    if (in == NULL || out == NULL || size != DGLAB_COYOTE_V3_B0_SIZE)
        return false;

    if (in[0] != DGLAB_COYOTE_V3_HEADER_B0)
        return false;

    uint8_t modes = in[B0_OFFSET_SEQUENCE];

    out->sequence = (uint8_t)(modes >> 4);
    out->strength_a.mode = (DglabCoyoteV3StrengthMode)((modes >> 2) & 0x03u);
    out->strength_b.mode = (DglabCoyoteV3StrengthMode)(modes & 0x03u);
    out->strength_a.value = in[B0_OFFSET_STRENGTH_A];
    out->strength_b.value = in[B0_OFFSET_STRENGTH_B];

    decodeChannelWaveform(in + B0_OFFSET_WAVEFORM_A_FREQ, in + B0_OFFSET_WAVEFORM_A_STRENGTH,
        out->waveform_a);
    decodeChannelWaveform(in + B0_OFFSET_WAVEFORM_B_FREQ, in + B0_OFFSET_WAVEFORM_B_STRENGTH,
        out->waveform_b);

    return true;
}

void dglabCoyoteV3EncodeBf(const DglabCoyoteV3Bf* params, uint8_t* out)
{
    out[0] = (uint8_t)DGLAB_COYOTE_V3_HEADER_BF;
    out[1] = params->soft_limit_a;
    out[2] = params->soft_limit_b;
    out[3] = params->frequency_balance_a;
    out[4] = params->frequency_balance_b;
    out[5] = params->strength_balance_a;
    out[6] = params->strength_balance_b;
}

bool dglabCoyoteV3DecodeBf(const uint8_t* in, size_t size, DglabCoyoteV3Bf* out)
{
    if (in == NULL || out == NULL || size != DGLAB_COYOTE_V3_BF_SIZE)
        return false;

    if (in[0] != DGLAB_COYOTE_V3_HEADER_BF)
        return false;

    out->soft_limit_a = in[1];
    out->soft_limit_b = in[2];
    out->frequency_balance_a = in[3];
    out->frequency_balance_b = in[4];
    out->strength_balance_a = in[5];
    out->strength_balance_b = in[6];

    return true;
}

bool dglabCoyoteV3DecodeB1(const uint8_t* in, size_t size, DglabCoyoteV3B1* out)
{
    if (in == NULL || out == NULL || size < DGLAB_COYOTE_V3_B1_SIZE)
        return false;

    if (in[0] != DGLAB_COYOTE_V3_HEADER_B1)
        return false;

    out->sequence = in[1];
    out->strength_a = in[2];
    out->strength_b = in[3];

    return true;
}
