// Host side tests for the motion to waveform feed.
//
// The mapping is the part of the Joy-Con mode that gets tuned by feel, so it
// lives in a platform independent file and is pinned down here: dead zone and
// hysteresis, peak-hold per 25ms window, the attack/release envelope, the
// frequency that follows the intensity, and the idle stop.

#include <dglab/nro/motion_feed.h>

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

#define SLOT_NS (DGLAB_NET_WAVEFORM_SLOT_MS * 1000000u)

static DglabMotionFeed g_feed;
static DglabNetWaveformSlot g_slots[DGLAB_NET_WAVEFORM_MAX_SLOTS];

static void resetFeed(void)
{
    DglabMotionFeedConfig config;

    dglabMotionFeedDefaultConfig(&config);
    dglabMotionFeedInit(&g_feed, &config);

    memset(g_slots, 0, sizeof(g_slots));
}

static DglabMotionSample stillSample(void)
{
    DglabMotionSample sample;

    memset(&sample, 0, sizeof(sample));

    // A hand that holds a controller is never perfectly still: this is noise
    // below the dead zone.
    sample.angular_velocity[0] = 0.1f;
    sample.acceleration[2] = 1.0f;
    sample.delta_time_us = 16000;

    return sample;
}

static DglabMotionSample swingSample(void)
{
    DglabMotionSample sample = stillSample();

    sample.angular_velocity[0] = 6.0f; // the configured full scale

    return sample;
}

// Feeds one sample and advances exactly one slot.
static size_t stepOne(DglabMotionSample sample, DglabNetWaveformSlot* out)
{
    dglabMotionFeedAddSample(&g_feed, &sample);

    return dglabMotionFeedAdvance(&g_feed, SLOT_NS, out, 1);
}

// A controller that stops being sampled altogether - turned off, or plugged back
// into the console - must not leave the row saying 挥动中 with a waveform value of
// 0: with no readings coming in, the envelope releases and the movement is over
// (hardware report, 2026-09-17: the rows showed 挥动中 / 0 / 100ms and stayed
// there).
static void testSilenceEndsTheMovement(void)
{
    DglabNetWaveformSlot slot;

    resetFeed();

    for (int i = 0; i < 8; i++)
        CHECK(stepOne(swingSample(), &slot) == 1);

    CHECK(dglabMotionFeedIsMoving(&g_feed));

    // Time passes and nothing at all arrives.
    for (int i = 0; i < 80 && dglabMotionFeedIsMoving(&g_feed); i++)
        (void)dglabMotionFeedAdvance(&g_feed, SLOT_NS, &slot, 1);

    CHECK(!dglabMotionFeedIsMoving(&g_feed));
    CHECK(!dglabMotionFeedIsStreaming(&g_feed));
    CHECK(slot.strength == 0);
    // The envelope is an exponential, so it approaches zero instead of hitting
    // it; the row shows the quantised value, which is 0.
    CHECK(dglabMotionFeedLevel(&g_feed) < 0.01f);
}

static void testStillHandIsSilent(void)
{
    DglabNetWaveformSlot slot;

    resetFeed();

    CHECK(!dglabMotionFeedIsStreaming(&g_feed));
    CHECK(!dglabMotionFeedIsMoving(&g_feed));

    // Ten slots of a hand that holds the controller: silence, slowest
    // frequency, and once past idle_stop_ms nothing should be uploaded at all.
    for (int i = 0; i < 10; i++) {
        CHECK(stepOne(stillSample(), &slot) == 1);
        CHECK(slot.strength == 0);
        CHECK(slot.frequency_ms == 100);
    }

    CHECK(!dglabMotionFeedIsStreaming(&g_feed));
    CHECK(dglabMotionFeedLevel(&g_feed) == 0.0f);
}

static void testSingleSwingPeaksThenDecays(void)
{
    DglabNetWaveformSlot slot;

    resetFeed();

    // One full scale swing inside a 25ms window: the envelope only gets halfway
    // to it in that window (attack is 50ms), and that is the point of measuring
    // the peak per window rather than sampling the value at the boundary.
    CHECK(stepOne(swingSample(), &slot) == 1);
    CHECK(slot.strength == 50);
    CHECK(slot.frequency_ms == 65); // halfway between 100ms and 30ms
    CHECK(dglabMotionFeedIsStreaming(&g_feed));
    CHECK(dglabMotionFeedIsMoving(&g_feed));

    // Then it releases: 25ms of every 300ms, towards zero.
    CHECK(stepOne(stillSample(), &slot) == 1);
    CHECK(slot.strength == 46);
    CHECK(stepOne(stillSample(), &slot) == 1);
    CHECK(slot.strength == 42);

    // The stream stops once the release has finished, not merely once the
    // movement stopped: cutting it earlier would cut the decay off. Bounded, so
    // a release that never finishes fails here instead of hanging.
    for (int i = 0; i < 200 && dglabMotionFeedIsStreaming(&g_feed); i++)
        (void)stepOne(stillSample(), &slot);

    CHECK(!dglabMotionFeedIsStreaming(&g_feed));
    CHECK(slot.strength == 0);
}

static void testSustainedMotionReachesFullScale(void)
{
    DglabNetWaveformSlot slot;

    resetFeed();

    for (int i = 0; i < 5; i++)
        CHECK(stepOne(swingSample(), &slot) == 1);

    // 0.5, 0.75, 0.875, 0.9375, 0.96875 of the way up.
    CHECK(slot.strength == 97);
    CHECK(slot.frequency_ms == 32);
    CHECK(dglabMotionFeedLevel(&g_feed) > 0.95f);
}

static void testDeadZoneHysteresis(void)
{
    DglabMotionSample sample = stillSample();
    DglabNetWaveformSlot slot;

    resetFeed();

    // Between the two thresholds while stopped: still stopped.
    sample.angular_velocity[0] = 0.18f; // 0.03 of full scale
    (void)stepOne(sample, &slot);
    CHECK(!dglabMotionFeedIsMoving(&g_feed));

    // Past the enter threshold: moving.
    sample.angular_velocity[0] = 0.4f; // 0.067 of full scale
    (void)stepOne(sample, &slot);
    CHECK(dglabMotionFeedIsMoving(&g_feed));

    // Back into the band: it stays "moving" instead of chattering at the edge.
    sample.angular_velocity[0] = 0.18f;
    (void)stepOne(sample, &slot);
    CHECK(dglabMotionFeedIsMoving(&g_feed));

    // Below the exit threshold it lets go.
    sample.angular_velocity[0] = 0.05f; // 0.008 of full scale
    (void)stepOne(sample, &slot);
    CHECK(!dglabMotionFeedIsMoving(&g_feed));
}

static void testInterpolatedSamplesAreIgnored(void)
{
    DglabMotionSample sample = swingSample();
    DglabNetWaveformSlot slot;

    resetFeed();

    sample.interpolated = true;

    CHECK(stepOne(sample, &slot) == 1);
    CHECK(slot.strength == 0);
    CHECK(!dglabMotionFeedIsMoving(&g_feed));
}

static void testAccelerationDeltaContributes(void)
{
    DglabMotionSample sample = stillSample();
    DglabNetWaveformSlot slot;

    resetFeed();

    // The first sample has nothing to compare against, so it cannot be a jerk.
    sample.acceleration[0] = 1.0f;
    (void)stepOne(sample, &slot);
    CHECK(!dglabMotionFeedIsMoving(&g_feed));

    // The same acceleration again is no change at all.
    (void)stepOne(sample, &slot);
    CHECK(!dglabMotionFeedIsMoving(&g_feed));

    // A step change is a jerk: 2.0 of change against a 2.0 reference.
    sample.acceleration[0] = 3.0f;
    (void)stepOne(sample, &slot);
    CHECK(dglabMotionFeedIsMoving(&g_feed));
}

static void testPacingFollowsTimeNotFrames(void)
{
    size_t count;

    resetFeed();

    // 10ms does not fill a slot; the leftover time is kept.
    CHECK(dglabMotionFeedAdvance(&g_feed, 10u * 1000000u, g_slots, 8) == 0);
    CHECK(dglabMotionFeedAdvance(&g_feed, 10u * 1000000u, g_slots, 8) == 0);
    CHECK(dglabMotionFeedAdvance(&g_feed, 10u * 1000000u, g_slots, 8) == 1);

    // A stall produces one slot per 25ms, up to the space the caller offers.
    count = dglabMotionFeedAdvance(&g_feed, 100u * 1000000u, g_slots, 8);
    CHECK(count == 4);

    count = dglabMotionFeedAdvance(&g_feed, 1000u * 1000000u, g_slots, 3);
    CHECK(count == 3);

    // Whatever did not fit is still owed, not dropped.
    count = dglabMotionFeedAdvance(&g_feed, 0, g_slots, DGLAB_NET_WAVEFORM_MAX_SLOTS);
    CHECK(count == 37);
}

static void testStrengthMaxScales(void)
{
    DglabMotionFeedConfig config;
    DglabNetWaveformSlot slot;

    dglabMotionFeedDefaultConfig(&config);
    config.strength_max = 40;
    dglabMotionFeedInit(&g_feed, &config);

    for (int i = 0; i < 8; i++)
        (void)stepOne(swingSample(), &slot);

    CHECK(slot.strength == 40);
}

static void testFrequencyGetterMatchesTheSlots(void)
{
    DglabNetWaveformSlot slot;

    resetFeed();

    CHECK(dglabMotionFeedFrequencyMs(&g_feed) == 100);

    for (int i = 0; i < 8; i++)
        (void)stepOne(swingSample(), &slot);

    // The getter is what the screen shows, so it has to agree with the slot.
    CHECK(dglabMotionFeedFrequencyMs(&g_feed) == slot.frequency_ms);
}

// The rule behind the motion page's two Joy-Con rows (docs/joycon-input.md).
// Readings only reach the feed when they reported being connected, so a sample
// is proof that the side is alive; and since a live sensor fills the LIFO every
// frame, no readings for a while is proof that it is gone. Nothing else is
// evidence - which is the point: a handle that claims to be connected while
// handing over nothing used to keep the row on 挥动中 forever.
static void testSensorSideConnection(void)
{
    DglabMotionSensorPoll poll;

    memset(&poll, 0, sizeof(poll));

    // Samples arrived, even though the other handle's placeholders are counted
    // in the same poll: readings win.
    poll.answered = true;
    poll.connected = false;
    poll.samples = 5;
    CHECK(dglabMotionSensorConnected(false, &poll, 0));

    // A handle answered with placeholders. That is not proof on its own: a
    // placeholder can turn up in the middle of a working session, so the row
    // waits for the quiet threshold instead of flipping on one poll.
    poll.samples = 0;
    CHECK(dglabMotionSensorConnected(true, &poll, 0));
    CHECK(!dglabMotionSensorConnected(true, &poll, DGLAB_MOTION_SENSOR_QUIET_POLLS));

    // Nothing answered at all, and not for long: the first frames after the mode
    // starts have nothing to show yet, so the row keeps what it had instead of
    // flickering.
    poll.answered = false;
    CHECK(dglabMotionSensorConnected(true, &poll, 0));
    CHECK(!dglabMotionSensorConnected(false, &poll, 0));
    CHECK(dglabMotionSensorConnected(true, &poll, DGLAB_MOTION_SENSOR_QUIET_POLLS - 1));

    // A Joy-Con that is turned off, or plugged back into the console, stops
    // delivering readings instead of announcing that it left: after a run of
    // empty polls the side has to count as gone (hardware report, 2026-09-17,
    // where the row stayed on "connected" for the whole session).
    CHECK(!dglabMotionSensorConnected(true, &poll, DGLAB_MOTION_SENSOR_QUIET_POLLS));
    CHECK(!dglabMotionSensorConnected(true, &poll, DGLAB_MOTION_SENSOR_QUIET_POLLS + 50));

    // Readings reset that count, so a controller that comes back is connected
    // again on the first poll that carries something.
    poll.answered = true;
    poll.connected = true;
    poll.samples = 1;
    CHECK(dglabMotionSensorConnected(false, &poll, 0));

    // The "connected" bit alone is not enough either (a handle that says it is
    // connected while everything it hands over is interpolated, or nothing at
    // all): no readings means the row is not going to stay on 挥动中.
    poll.answered = true;
    poll.connected = true;
    poll.samples = 0;
    CHECK(dglabMotionSensorConnected(true, &poll, DGLAB_MOTION_SENSOR_QUIET_POLLS - 1));
    CHECK(!dglabMotionSensorConnected(true, &poll, DGLAB_MOTION_SENSOR_QUIET_POLLS));

    // A side the console does not present as a usable detached Joy-Con (clipped
    // onto the console, switched off, or gone) is not connected whatever the
    // handles have to say: they are not even asked (hardware report, 2026-09-17 -
    // they happily hand over readings for a Joy-Con that is attached or off, and
    // that is what kept the rows on 挥动中).
    poll.not_usable = true;
    poll.answered = true;
    poll.connected = true;
    poll.samples = 9;
    CHECK(!dglabMotionSensorConnected(true, &poll, 0));

    // And no poll at all is not evidence either.
    CHECK(dglabMotionSensorConnected(true, NULL, 999));
}

int main(void)
{
    testStillHandIsSilent();
    testSilenceEndsTheMovement();
    testSingleSwingPeaksThenDecays();
    testSustainedMotionReachesFullScale();
    testDeadZoneHysteresis();
    testInterpolatedSamplesAreIgnored();
    testAccelerationDeltaContributes();
    testPacingFollowsTimeNotFrames();
    testStrengthMaxScales();
    testFrequencyGetterMatchesTheSlots();
    testSensorSideConnection();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
