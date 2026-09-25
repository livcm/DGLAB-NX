#include <dglab/nro/motion_feed.h>

#include <math.h>
#include <string.h>

// One slot is 25ms of output; the constant comes from the IPC surface so the
// producer and the server's pacing cannot drift apart.
#define SLOT_MS DGLAB_NET_WAVEFORM_SLOT_MS
#define NS_PER_MS 1000000u

void dglabMotionFeedDefaultConfig(DglabMotionFeedConfig* config)
{
    if (!config)
        return;

    memset(config, 0, sizeof(*config));

    // The references are the "full scale" magnitudes: what a deliberate swing is
    // worth. They are first guesses and are meant to be tuned on hardware.
    config->gyro_weight = 1.0f;
    config->gyro_reference = 6.0f;
    config->accel_weight = 1.0f;
    config->accel_reference = 2.0f;

    config->deadzone_enter = 0.05f;
    config->deadzone_exit = 0.02f;

    config->attack_ms = 50u;
    config->release_ms = 300u;
    config->idle_stop_ms = 250u;

    config->frequency_still_ms = 100u;
    config->frequency_fast_ms = 30u;

    // The density starts out variable, and the fixed interval is the midpoint of
    // the two ends above - a first guess that is meant to be tuned on hardware.
    config->density_fixed = false;
    config->frequency_fixed_ms = 65u;

    config->strength_max = 100u;

    // Both channel strength ceilings start wide open. They only ever cap a
    // strength, and every strength starts at 0, so a fresh session still cannot
    // output anything until somebody dials one up - the ceiling is the safety
    // limit a user sets when they want one, not the thing that keeps the device
    // quiet.
    config->channel_limit_a = 100u;
    config->channel_limit_b = 100u;

    // The motion mode's own shape: it only ever writes a level, and the pulse
    // interval follows it.
    config->frequency_follows_level = true;
}

void dglabMotionFeedInit(DglabMotionFeed* feed, const DglabMotionFeedConfig* config)
{
    DglabMotionFeedConfig defaults;

    if (!feed)
        return;

    dglabMotionFeedDefaultConfig(&defaults);

    memset(feed, 0, sizeof(*feed));
    feed->config = config ? *config : defaults;

    // A controller that is simply held still must not produce anything, so the
    // idle state is where a session starts.
    feed->streaming = false;
}

static float vectorLength(const float values[3])
{
    return sqrtf(values[0] * values[0] + values[1] * values[1] + values[2] * values[2]);
}

static float clamp01(float value)
{
    if (value < 0.0f)
        return 0.0f;

    if (value > 1.0f)
        return 1.0f;

    return value;
}

// How much of the "movement" this sample carries, 0..1, dead zone applied.
static float sampleIntensity(DglabMotionFeed* feed, const DglabMotionSample* sample)
{
    const DglabMotionFeedConfig* config = &feed->config;
    float rotation = vectorLength(sample->angular_velocity) / config->gyro_reference;
    float jerk = 0.0f;
    float raw;

    if (feed->have_last_acceleration) {
        float delta[3];

        for (int i = 0; i < 3; i++)
            delta[i] = sample->acceleration[i] - feed->last_acceleration[i];

        jerk = vectorLength(delta) / config->accel_reference;
    }

    raw = clamp01(config->gyro_weight * rotation + config->accel_weight * jerk);

    // Hysteresis: entering needs more than staying, so a hand tremor that sits
    // right on the threshold does not switch the stream on and off.
    if (raw >= config->deadzone_enter)
        feed->moving = true;
    else if (raw <= config->deadzone_exit)
        feed->moving = false;

    return feed->moving ? raw : 0.0f;
}

void dglabMotionFeedAddSample(DglabMotionFeed* feed, const DglabMotionSample* sample)
{
    float intensity;

    if (!feed || !sample)
        return;

    // Interpolated samples are made up by the platform, not measured: letting
    // them through would smooth away exactly the spikes this mode is for.
    if (sample->interpolated)
        return;

    intensity = sampleIntensity(feed, sample);

    if (intensity > feed->window_peak)
        feed->window_peak = intensity;

    // Remembered for the acceleration delta of the next sample, dead zone or
    // not: the difference is what matters, not the gating.
    memcpy(feed->last_acceleration, sample->acceleration, sizeof(feed->last_acceleration));
    feed->have_last_acceleration = true;
}

void dglabMotionFeedSetTarget(DglabMotionFeed* feed, float level, float density)
{
    if (!feed)
        return;

    level = clamp01(level);
    density = clamp01(density);

    // The window keeps the highest level it was asked for, exactly like it keeps
    // the highest sample intensity. The density is simply the last one asked for:
    // it is where a finger is, and smoothing that would only make the output
    // disagree with the marker the page draws under it.
    if (level > feed->window_peak)
        feed->window_peak = level;

    feed->window_density = density;
    feed->moving = level > 0.0f;
}

// The pulse interval for the window that is going out. With the shared density
// switch on it is one number whatever the input is doing; otherwise the motion
// mode's density is its own level (a harder swing is also a denser one), and a
// mode that names targets gets the interval it asked for.
static uint16_t frequencyMs(const DglabMotionFeed* feed, float level)
{
    float norm;
    float frequency;

    if (feed->config.density_fixed)
        return feed->config.frequency_fixed_ms;

    norm = feed->config.frequency_follows_level ? clamp01(level)
                                                : clamp01(feed->window_density);
    frequency = (float)feed->config.frequency_still_ms +
        ((float)feed->config.frequency_fast_ms - (float)feed->config.frequency_still_ms) * norm;

    return (uint16_t)(frequency + 0.5f);
}

// Moves the envelope towards `target`, using the attack time when rising and the
// release time when falling.
static void envelopeStep(DglabMotionFeed* feed, float target, uint32_t step_ms)
{
    uint32_t tau = (target > feed->level) ? feed->config.attack_ms : feed->config.release_ms;
    float step;

    if (tau == 0) {
        feed->level = target;
        return;
    }

    step = (float)step_ms / (float)tau;

    if (step > 1.0f)
        step = 1.0f;

    feed->level += (target - feed->level) * step;
}

size_t dglabMotionFeedAdvance(DglabMotionFeed* feed, uint32_t elapsed_ns,
    DglabNetWaveformSlot* slots, size_t max)
{
    size_t written = 0;

    if (!feed || !slots || max == 0)
        return 0;

    feed->window_ns += elapsed_ns;

    while (feed->window_ns >= SLOT_MS * NS_PER_MS && written < max) {
        // The peak of the window is what goes out, not the average: a single
        // swing inside 25ms has to be felt, not diluted by the quiet samples
        // around it.
        float target = feed->moving ? feed->window_peak : 0.0f;
        bool saw_peak = feed->window_peak > 0.0f;
        float level;

        feed->window_ns -= SLOT_MS * NS_PER_MS;

        envelopeStep(feed, target, SLOT_MS);

        if (feed->window_peak > 0.0f)
            feed->idle_ms = 0;
        else if (feed->idle_ms < feed->config.idle_stop_ms)
            feed->idle_ms += SLOT_MS;

        feed->window_peak = 0.0f;

        level = clamp01(feed->level);
        slots[written].strength = (u8)(level * (float)feed->config.strength_max + 0.5f);
        slots[written].frequency_ms = frequencyMs(feed, level);
        slots[written].pad = 0;
        written++;

        // A hand that stops being sampled at all - the controller was turned off,
        // or plugged back into the console - used to leave `moving` set, so the
        // row kept saying 挥动中 with a waveform value of 0 and the still
        // frequency until the mode was left. Once the release has finished (the
        // quantised strength is zero, the same measure the streaming rule uses)
        // and nothing came in during the window, the movement is over.
        if (!saw_peak && slots[written - 1].strength == 0)
            feed->moving = false;

        // Once the hand has been still past idle_stop_ms *and* the release has
        // decayed all the way down to a zero strength, there is nothing left to
        // say and the producer can stop uploading. Waiting for the zero matters:
        // stopping while the envelope is still up would cut the release off.
        feed->streaming = !(feed->idle_ms >= feed->config.idle_stop_ms &&
                            slots[written - 1].strength == 0);
    }

    // Time that did not fill a slot is kept for the next call.
    return written;
}

float dglabMotionFeedLevel(const DglabMotionFeed* feed)
{
    if (!feed)
        return 0.0f;

    return clamp01(feed->level);
}

uint16_t dglabMotionFeedFrequencyMs(const DglabMotionFeed* feed)
{
    if (!feed)
        return 0;

    return frequencyMs(feed, feed->level);
}

bool dglabMotionFeedIsStreaming(const DglabMotionFeed* feed)
{
    return feed ? feed->streaming : false;
}

bool dglabMotionFeedIsMoving(const DglabMotionFeed* feed)
{
    return feed ? feed->moving : false;
}

bool dglabMotionSensorConnected(bool previous, const DglabMotionSensorPoll* poll,
    unsigned quiet_polls)
{
    if (!poll)
        return previous;

    // The console does not present this side as a usable detached Joy-Con (it is
    // attached, switched off, or gone): nothing to wait for.
    if (poll->not_usable)
        return false;

    // Readings only get this far when they reported being connected, so a sample
    // is proof that the side is alive - and the row can never contradict the
    // output the user is feeling.
    if (poll->samples > 0)
        return true;

    // Nothing else counts. A handle that says IsConnected but hands over no
    // readings, a batch of nothing but placeholders, and a handle that says
    // nothing at all are all the same thing for the row: no readings, so nothing
    // is driving the channel. The "connected" bit alone used to be enough, and
    // that is what kept a side on 挥动中 for the rest of the session once it had
    // been there (hardware report, 2026-09-17) - a live sensor fills the LIFO
    // every frame, so silence is a controller that went away.
    if (quiet_polls >= DGLAB_MOTION_SENSOR_QUIET_POLLS)
        return false;

    return previous;
}
