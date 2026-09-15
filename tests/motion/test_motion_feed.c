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

int main(void)
{
    testStillHandIsSilent();
    testSingleSwingPeaksThenDecays();
    testSustainedMotionReachesFullScale();
    testDeadZoneHysteresis();
    testInterpolatedSamplesAreIgnored();
    testAccelerationDeltaContributes();
    testPacingFollowsTimeNotFrames();
    testStrengthMaxScales();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
