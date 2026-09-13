#include <dglab/protocol/coyote_v3_session.h>

#include <string.h>

static void emit(DglabCoyoteV3Session* session, const uint8_t* data, size_t size)
{
    if (session->link.write != NULL)
        session->link.write(session->link.context, data, size);
}

static DglabCoyoteV3WaveformChannel* waveformFor(DglabCoyoteV3Session* session,
    DglabCoyoteV3Channel channel)
{
    return (channel == DglabCoyoteV3ChannelA) ? &session->waveform_a : &session->waveform_b;
}

static bool isEntryValid(const DglabCoyoteV3WaveformEntry* entry)
{
    return entry->frequency_ms >= DGLAB_COYOTE_V3_WAVEFORM_FREQUENCY_MIN_MS &&
           entry->frequency_ms <= DGLAB_COYOTE_V3_WAVEFORM_FREQUENCY_MAX_MS &&
           entry->strength <= DGLAB_COYOTE_V3_WAVEFORM_STRENGTH_MAX;
}

// Fills one packet's four waveform slots for a channel.
//
// The waveform repeats, and each repetition starts on a packet boundary. When a
// repetition ends in the middle of a packet the remaining slots are padded with
// the last frequency and strength 0.
//
// The documentation describes that padding as "不足的 0 补". Writing a zero
// frequency here would be out of the 10..240 range, which makes the device
// discard the channel's whole packet and would mute the valid entries in front
// of it, so the padding keeps the previous frequency and only zeroes the
// strength. A channel without waveform data keeps the zeroed slots, which is
// the documented single channel technique.
static void fillChannelWaveform(DglabCoyoteV3WaveformChannel* waveform,
    DglabCoyoteV3WaveformSlot* slots)
{
    if (waveform->count == 0)
        return;

    size_t filled = 0;
    uint8_t last_frequency = DGLAB_COYOTE_V3_WAVEFORM_FREQ_MIN;

    while (filled < DGLAB_COYOTE_V3_WAVEFORM_SLOTS) {
        if (waveform->position >= waveform->count) {
            if (filled == 0) {
                // A new repetition starts exactly on this packet boundary.
                waveform->position = 0;
                continue;
            }

            break;
        }

        const DglabCoyoteV3WaveformEntry* entry = &waveform->entries[waveform->position];
        waveform->position++;

        last_frequency = dglabCoyoteV3CompressFrequency(entry->frequency_ms);
        slots[filled].frequency = last_frequency;
        slots[filled].strength = entry->strength;
        filled++;
    }

    for (; filled < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; filled++) {
        slots[filled].frequency = last_frequency;
        slots[filled].strength = 0;
    }
}

static void writeB0(DglabCoyoteV3Session* session)
{
    DglabCoyoteV3B0 packet = { 0 };
    uint8_t encoded[DGLAB_COYOTE_V3_B0_SIZE];

    dglabCoyoteV3StrengthStateApplyToPacket(&session->strength, &packet);
    fillChannelWaveform(&session->waveform_a, packet.waveform_a);
    fillChannelWaveform(&session->waveform_b, packet.waveform_b);

    dglabCoyoteV3EncodeB0(&packet, encoded);
    emit(session, encoded, sizeof(encoded));
}

static void writeBf(DglabCoyoteV3Session* session)
{
    uint8_t encoded[DGLAB_COYOTE_V3_BF_SIZE];

    dglabCoyoteV3EncodeBf(&session->bf, encoded);
    emit(session, encoded, sizeof(encoded));
}

static void resetPlayback(DglabCoyoteV3Session* session)
{
    session->waveform_a.position = 0;
    session->waveform_b.position = 0;
}

void dglabCoyoteV3SessionInit(DglabCoyoteV3Session* session, const DglabCoyoteV3Link* link,
    const DglabCoyoteV3SessionConfig* config)
{
    DglabCoyoteV3SessionConfig defaults = { 0 };

    if (config != NULL)
        defaults = *config;

    memset(session, 0, sizeof(*session));

    if (link != NULL)
        session->link = *link;

    session->bf = defaults.bf;
    session->strength_feedback = defaults.strength_feedback;

    dglabCoyoteV3StrengthStateInit(&session->strength, defaults.strength_feedback);
}

void dglabCoyoteV3SessionOnConnected(DglabCoyoteV3Session* session)
{
    session->connected = true;
    session->elapsed_ms = 0;
    dglabCoyoteV3StrengthStateInit(&session->strength, session->strength_feedback);
    resetPlayback(session);

    // The device keeps its soft limits across reconnects, but the documentation
    // still requires writing them again so the session knows what they are.
    writeBf(session);
}

void dglabCoyoteV3SessionOnDisconnected(DglabCoyoteV3Session* session)
{
    session->connected = false;
    session->elapsed_ms = 0;
    dglabCoyoteV3StrengthStateInit(&session->strength, session->strength_feedback);
    resetPlayback(session);
}

bool dglabCoyoteV3SessionIsConnected(const DglabCoyoteV3Session* session)
{
    return session->connected;
}

void dglabCoyoteV3SessionOnNotification(DglabCoyoteV3Session* session, const uint8_t* data,
    size_t size)
{
    DglabCoyoteV3B1 notification = { 0 };

    if (!dglabCoyoteV3DecodeB1(data, size, &notification))
        return;

    dglabCoyoteV3StrengthStateOnB1(&session->strength, &notification);
}

void dglabCoyoteV3SessionAdjustStrength(DglabCoyoteV3Session* session,
    DglabCoyoteV3Channel channel, int32_t delta)
{
    if (!session->connected)
        return;

    dglabCoyoteV3StrengthStateAdjust(&session->strength, channel, delta);
}

void dglabCoyoteV3SessionSetStrengthZero(DglabCoyoteV3Session* session,
    DglabCoyoteV3Channel channel)
{
    if (!session->connected)
        return;

    dglabCoyoteV3StrengthStateRequestZero(&session->strength, channel);
}

bool dglabCoyoteV3SessionSetWaveform(DglabCoyoteV3Session* session,
    DglabCoyoteV3Channel channel, const DglabCoyoteV3WaveformEntry* entries, size_t count)
{
    if (entries == NULL || count == 0 || count > DGLAB_COYOTE_V3_WAVEFORM_CAPACITY)
        return false;

    for (size_t i = 0; i < count; i++) {
        if (!isEntryValid(&entries[i]))
            return false;
    }

    DglabCoyoteV3WaveformChannel* waveform = waveformFor(session, channel);

    memcpy(waveform->entries, entries, count * sizeof(*entries));
    waveform->count = count;
    waveform->position = 0;

    return true;
}

void dglabCoyoteV3SessionClearWaveform(DglabCoyoteV3Session* session,
    DglabCoyoteV3Channel channel)
{
    DglabCoyoteV3WaveformChannel* waveform = waveformFor(session, channel);

    waveform->count = 0;
    waveform->position = 0;
}

void dglabCoyoteV3SessionTick(DglabCoyoteV3Session* session, uint32_t elapsed_ms)
{
    if (!session->connected)
        return;

    session->elapsed_ms += elapsed_ms;

    if (session->elapsed_ms < DGLAB_COYOTE_V3_OUTPUT_INTERVAL_MS)
        return;

    session->elapsed_ms -= DGLAB_COYOTE_V3_OUTPUT_INTERVAL_MS;

    // Drop any further missed intervals instead of writing them back to back.
    if (session->elapsed_ms >= DGLAB_COYOTE_V3_OUTPUT_INTERVAL_MS)
        session->elapsed_ms %= DGLAB_COYOTE_V3_OUTPUT_INTERVAL_MS;

    writeB0(session);
}
