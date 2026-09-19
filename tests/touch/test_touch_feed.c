// Host side tests for the touch mode's mapping.
//
// The mode is "a position in, a waveform value and a pulse density out", and
// that is the part of it a session with a finger on the panel would otherwise be
// the only way to check - so the mapping lives in a platform independent file and
// is pinned down here: the region split, both axes and their ends, which finger
// owns a half, what a lift or a crossing of the middle does, and that the
// parameters it runs on are the motion mode's own (one file, one config).

#include <dglab/nro/motion_feed.h>
#include <dglab/nro/motion_settings.h>
#include <dglab/nro/touch_feed.h>

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

static DglabTouchFrame g_frame;
static DglabTouchFeed g_feed;
static DglabMotionFeed g_stream_a;
static DglabMotionFeed g_stream_b;
static DglabNetWaveformSlot g_slots[DGLAB_NET_WAVEFORM_MAX_SLOTS];

// The two halves as the tests talk about them: what main.c calls channel A and B
// of the panel, which drive the DG-LAB channels 1 and 2.
#define HALF_A DglabTouchChannelValue_A
#define HALF_B DglabTouchChannelValue_B

static void addPoint(uint32_t x, uint32_t y, uint32_t finger_id, bool start, bool end)
{
    DglabTouchPoint* point = &g_frame.points[g_frame.count++];

    point->x = x;
    point->y = y;
    point->finger_id = finger_id;
    point->start = start;
    point->end = end;
}

// One frame of the panel: every test builds the readings it wants and hands them
// over the way the platform layer does.
static void applyFrame(void)
{
    dglabTouchFeedUpdate(&g_feed, &g_frame);
    memset(&g_frame, 0, sizeof(g_frame));
}

// A finger that has been down for a while: no edges, just a position.
static void holdAt(uint32_t x, uint32_t y, uint32_t finger_id)
{
    addPoint(x, y, finger_id, false, false);
    applyFrame();
}

// Puts a finger down: the panel sets Start on the snapshot it appears in.
static void pressAt(uint32_t x, uint32_t y, uint32_t finger_id)
{
    addPoint(x, y, finger_id, true, false);
    applyFrame();
}

static void releaseAll(void)
{
    applyFrame();
}

static const DglabTouchChannelState* half(unsigned channel)
{
    return dglabTouchFeedChannel(&g_feed, channel);
}

// One 25ms slot, exactly as main.c produces them: every half with a finger names
// its target, then the clock moves on.
static size_t stepSlot(DglabMotionFeed* stream, unsigned channel)
{
    const DglabTouchChannelState* state = half(channel);

    if (state->held)
        dglabMotionFeedSetTarget(stream, state->level, state->density);

    return dglabMotionFeedAdvance(stream, SLOT_NS, g_slots, DGLAB_NET_WAVEFORM_MAX_SLOTS);
}

// The slot producer as the touch mode configures it: the same config the motion
// mode loads from motion.cfg, with the one thing the mode decides itself - that
// the density comes from the panel and not from the level.
static void resetStreams(DglabMotionFeedConfig* config)
{
    dglabMotionFeedInit(&g_stream_a, config);
    dglabMotionFeedInit(&g_stream_b, config);
    memset(g_slots, 0, sizeof(g_slots));
}

static void resetFeed(void)
{
    memset(&g_frame, 0, sizeof(g_frame));
    dglabTouchFeedInit(&g_feed);
}

// x < 640 is channel A, x >= 640 is channel B: the split is the middle of the
// panel, and both of its edges belong to the halves beside them.
static void testRegionSplit(void)
{
    static const uint32_t lefts[] = { 0, 1, 639 };
    static const uint32_t rights[] = { 640, 641, 1279 };

    for (size_t i = 0; i < sizeof(lefts) / sizeof(lefts[0]); i++) {
        resetFeed();
        holdAt(lefts[i], 300, 1);

        CHECK(half(HALF_A)->held);
        CHECK(!half(HALF_B)->held);
        CHECK(half(HALF_A)->x == lefts[i]);
    }

    for (size_t i = 0; i < sizeof(rights) / sizeof(rights[0]); i++) {
        resetFeed();
        holdAt(rights[i], 300, 1);

        CHECK(!half(HALF_A)->held);
        CHECK(half(HALF_B)->held);
        CHECK(half(HALF_B)->x == rights[i]);
    }
}

// The two rules the page draws are the ends of the value axis; beyond them the
// reading is clamped, so a finger on the header is full value and one on the
// bottom bar is silence rather than "nothing".
static void testValueAxis(void)
{
    resetFeed();
    CHECK(dglabTouchLevelForY(DGLAB_TOUCH_VALUE_TOP) == 1.0f);
    CHECK(dglabTouchLevelForY(DGLAB_TOUCH_VALUE_BOTTOM) == 0.0f);
    CHECK(dglabTouchLevelForY((DGLAB_TOUCH_VALUE_TOP + DGLAB_TOUCH_VALUE_BOTTOM) / 2) == 0.5f);
    CHECK(dglabTouchLevelForY(0) == 1.0f);
    CHECK(dglabTouchLevelForY(DGLAB_TOUCH_PANEL_HEIGHT - 1) == 0.0f);

    // And through the feed, a finger at the top of the panel is a full value.
    holdAt(100, DGLAB_TOUCH_VALUE_TOP, 1);
    CHECK(half(HALF_A)->held);
    CHECK(half(HALF_A)->level == 1.0f);

    resetFeed();
    holdAt(100, DGLAB_TOUCH_VALUE_BOTTOM, 1);
    CHECK(half(HALF_A)->held);
    CHECK(half(HALF_A)->level == 0.0f);
}

// The horizontal axis runs across each half on its own, and its ends are page
// geometry rather than the edges of the screen: the left half runs from the first
// column of the two white rules to the column beside the centre line, the right
// half from the centre line to the last column of the rules. A fingertip cannot
// reach the panel's own edge, and an axis drawn to it never produced the
// frequency parameters at all (docs/touch-input.md, hardware report 2026-09-19).
// Outside an axis the reading is clamped, so pushing past an end still asks for
// that end's extreme.
static void testDensityAxis(void)
{
    resetFeed();

    // The left half.
    CHECK(dglabTouchDensityForX(DGLAB_TOUCH_DENSITY_LEFT) == 0.0f);
    CHECK(dglabTouchDensityForX(DGLAB_TOUCH_SPLIT - 1) == 1.0f);
    CHECK(dglabTouchDensityForX(0) == 0.0f);
    CHECK(dglabTouchDensityForX(DGLAB_TOUCH_DENSITY_LEFT - 1) == 0.0f);

    // The right half, the same the other way round.
    CHECK(dglabTouchDensityForX(DGLAB_TOUCH_SPLIT) == 0.0f);
    CHECK(dglabTouchDensityForX(DGLAB_TOUCH_DENSITY_RIGHT) == 1.0f);
    CHECK(dglabTouchDensityForX(DGLAB_TOUCH_DENSITY_RIGHT + 1) == 1.0f);
    CHECK(dglabTouchDensityForX(DGLAB_TOUCH_PANEL_WIDTH - 1) == 1.0f);

    // The three ticks the page draws are the quarters of that axis, so they read
    // as 25% / 50% / 75% of the range they divide.
    {
        static const struct {
            uint32_t x;
            float quarter;
        } ticks[] = {
            { 177, 0.25f }, { 331, 0.50f }, { 485, 0.75f },  // left half
            { 793, 0.25f }, { 947, 0.50f }, { 1101, 0.75f }, // right half
        };

        for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); i++) {
            float density = dglabTouchDensityForX(ticks[i].x);

            CHECK(density > ticks[i].quarter - 0.01f && density < ticks[i].quarter + 0.01f);
        }
    }

    // And through the feed: the same numbers, and the two clamp bands reach the
    // extremes rather than something near them.
    holdAt(DGLAB_TOUCH_SPLIT - 1, 300, 1);
    CHECK(half(HALF_A)->density == 1.0f);

    resetFeed();
    holdAt(0, 300, 1);
    CHECK(half(HALF_A)->held);
    CHECK(half(HALF_A)->density == 0.0f);

    resetFeed();
    holdAt(DGLAB_TOUCH_SPLIT, 300, 1);
    CHECK(half(HALF_B)->density == 0.0f);

    resetFeed();
    holdAt(DGLAB_TOUCH_PANEL_WIDTH - 1, 300, 1);
    CHECK(half(HALF_B)->held);
    CHECK(half(HALF_B)->density == 1.0f);
}

// Two fingers, one per half: both halves are driven at once, each with its own
// position - this is what the whole layout is for.
static void testOneFingerPerHalfAtOnce(void)
{
    resetFeed();

    addPoint(100, 200, 1, true, false);
    addPoint(900, 500, 2, true, false);
    applyFrame();

    CHECK(half(HALF_A)->held);
    CHECK(half(HALF_B)->held);
    CHECK(half(HALF_A)->x == 100);
    CHECK(half(HALF_B)->x == 900);
    CHECK(half(HALF_A)->level > half(HALF_B)->level); // 200 is higher up than 500
    CHECK(half(HALF_B)->density > half(HALF_A)->density); // 900 is further right
}

// "The last finger put down owns the half": a second finger in the same half
// takes it over, and the first one lifting afterwards changes nothing.
static void testSecondFingerTakesTheHalf(void)
{
    resetFeed();
    pressAt(100, 600, 1);

    CHECK(half(HALF_A)->x == 100);

    addPoint(300, 600, 1, false, false);
    addPoint(300, 200, 2, true, false);
    applyFrame();

    CHECK(half(HALF_A)->held);
    CHECK(half(HALF_A)->x == 300);

    // The first finger is gone (the panel simply stops listing it) and the second
    // one is still down: the half follows the second one.
    holdAt(300, 200, 2);
    CHECK(half(HALF_A)->held);
    CHECK(half(HALF_A)->x == 300);
}

// The other side of the same rule: when the finger that owns a half lifts and
// another one is still in that half, the half does not go silent - it follows the
// finger that is still there.
static void testLiftHandsTheHalfBack(void)
{
    resetFeed();
    pressAt(100, 200, 1);

    addPoint(100, 200, 1, false, false);
    addPoint(500, 200, 2, true, false);
    applyFrame();

    CHECK(half(HALF_A)->x == 500);

    holdAt(100, 200, 1);
    CHECK(half(HALF_A)->held);
    CHECK(half(HALF_A)->x == 100);
}

// A reading the panel marks as ended does not own a half, and an empty frame
// releases both - the level and the density stay where the finger left them, so
// the release curve runs from the last position instead of from silence.
static void testLiftReleasesTheHalf(void)
{
    resetFeed();
    holdAt(400, 300, 1);

    CHECK(half(HALF_A)->held);

    {
        float level = half(HALF_A)->level;
        float density = half(HALF_A)->density;

        addPoint(400, 300, 1, false, true);
        applyFrame();

        CHECK(!half(HALF_A)->held);
        CHECK(half(HALF_A)->level == level);
        CHECK(half(HALF_A)->density == density);
        CHECK(half(HALF_A)->x == 400);
    }

    resetFeed();
    holdAt(400, 300, 1);
    releaseAll();

    CHECK(!half(HALF_A)->held);
    CHECK(!half(HALF_B)->held);
}

// Dragging across the middle: the half the finger left releases, and the other
// one takes it over. The two halves are the two channels, so this is what
// "channel A lets go while channel B starts" looks like.
static void testCrossingTheMiddle(void)
{
    resetFeed();
    holdAt(600, 300, 1);
    CHECK(half(HALF_A)->held);
    CHECK(!half(HALF_B)->held);

    holdAt(700, 300, 1);
    CHECK(!half(HALF_A)->held);
    CHECK(half(HALF_B)->held);
    CHECK(half(HALF_B)->x == 700);
}

// The point of sharing the feed: the panel's value and density come out as the
// slots the sysmodule streams, with the parameters of the file both modes read.
static void testPositionsBecomeSlots(void)
{
    DglabMotionFeedConfig config;
    size_t produced;

    dglabMotionSettingsDefault(&config);
    config.frequency_follows_level = false;
    resetStreams(&config);

    // Top right of the left half: full value, densest pulses.
    resetFeed();
    holdAt(DGLAB_TOUCH_SPLIT - 1, DGLAB_TOUCH_VALUE_TOP, 1);

    for (int i = 0; i < 40; i++)
        produced = stepSlot(&g_stream_a, HALF_A);

    CHECK(produced == 1);
    CHECK(g_slots[0].strength == config.strength_max);
    CHECK(g_slots[0].frequency_ms == config.frequency_fast_ms);

    // Bottom left: silence, and the density is at the other end of the range. A
    // fresh producer, so this measures the position and not the release of the
    // full-value case above it.
    resetFeed();
    resetStreams(&config);
    holdAt(0, DGLAB_TOUCH_VALUE_BOTTOM, 1);

    for (int i = 0; i < 40; i++)
        produced = stepSlot(&g_stream_a, HALF_A);

    CHECK(produced == 1);
    CHECK(g_slots[0].strength == 0);
    CHECK(g_slots[0].frequency_ms == config.frequency_still_ms);
}

// The density is its own axis: a light touch on the right of a half is a small
// value with dense pulses, which the motion mode's "density follows the value"
// shape could not express.
static void testDensityIsNotTheValue(void)
{
    DglabMotionFeedConfig config;

    dglabMotionSettingsDefault(&config);
    config.frequency_follows_level = false;
    resetStreams(&config);

    resetFeed();
    // Near the bottom of the value axis, at the right end of the left half's
    // density axis.
    holdAt(DGLAB_TOUCH_SPLIT - 1, DGLAB_TOUCH_VALUE_BOTTOM - 56, 1);

    for (int i = 0; i < 40; i++)
        stepSlot(&g_stream_a, HALF_A);

    CHECK(g_slots[0].strength > 0);
    CHECK(g_slots[0].strength < 20);
    CHECK(g_slots[0].frequency_ms == config.frequency_fast_ms);
}

// Changing the shared parameters changes the touch mode's output: they are the
// same numbers the Advanced page edits for the motion mode.
static void testSharedParameters(void)
{
    DglabMotionFeedConfig config;

    dglabMotionSettingsDefault(&config);
    config.frequency_follows_level = false;
    config.frequency_fast_ms = 10;
    config.strength_max = 80;
    resetStreams(&config);

    resetFeed();
    holdAt(DGLAB_TOUCH_SPLIT - 1, DGLAB_TOUCH_VALUE_TOP, 1);

    for (int i = 0; i < 40; i++)
        stepSlot(&g_stream_a, HALF_A);

    CHECK(g_slots[0].frequency_ms == 10);
    CHECK(g_slots[0].strength == 80);
}

// Lifting the finger runs the release curve and then stops uploading, exactly the
// way a controller that stops being sampled does in the motion mode - and the
// pulse interval stays where the finger left it while the value decays.
static void testReleaseThenStop(void)
{
    DglabMotionFeedConfig config;
    DglabNetWaveformSlot last;
    int slots = 0;

    dglabMotionSettingsDefault(&config);
    config.frequency_follows_level = false;
    resetStreams(&config);

    resetFeed();
    pressAt(100, 300, 1);

    for (int i = 0; i < 20; i++)
        stepSlot(&g_stream_a, HALF_A);

    CHECK(g_slots[0].strength > 0);
    CHECK(dglabMotionFeedIsStreaming(&g_stream_a));

    memset(&last, 0, sizeof(last));

    // The finger is gone; nothing writes a target any more. The mode keeps
    // advancing the clock, which is what turns the release into real slots.
    releaseAll();

    CHECK(!half(HALF_A)->held);

    for (int i = 0; i < 200; i++) {
        size_t produced = stepSlot(&g_stream_a, HALF_A);

        if (produced > 0) {
            last = g_slots[0];
            slots++;
        }

        if (!dglabMotionFeedIsStreaming(&g_stream_a))
            break;
    }

    CHECK(slots > 0);
    CHECK(last.strength == 0);
    CHECK(!dglabMotionFeedIsStreaming(&g_stream_a));
    CHECK(dglabMotionFeedLevel(&g_stream_a) < 0.01f);
    // The density was not reset by the lift: the last interval keeps going out
    // while the value falls.
    CHECK(last.frequency_ms < config.frequency_still_ms);
}

// Past either end of the density axis the reading is clamped rather than ignored:
// a finger that cannot quite reach the end of the axis still asks for that end's
// pulse interval, which is exactly why the axis ends where the two rules do
// instead of at the edge of the panel.
static void testClampBandsReachTheEnds(void)
{
    DglabMotionFeedConfig config;

    dglabMotionSettingsDefault(&config);
    config.frequency_follows_level = false;

    // Left of the left rule's end: the sparsest interval, not silence and not
    // something near the end.
    resetStreams(&config);
    resetFeed();
    holdAt(0, DGLAB_TOUCH_VALUE_TOP, 1);

    for (int i = 0; i < 40; i++)
        stepSlot(&g_stream_a, HALF_A);

    CHECK(g_slots[0].strength == config.strength_max);
    CHECK(g_slots[0].frequency_ms == config.frequency_still_ms);

    // Right of the right rule's end: the densest one, on the right half.
    resetStreams(&config);
    resetFeed();
    holdAt(DGLAB_TOUCH_PANEL_WIDTH - 1, DGLAB_TOUCH_VALUE_TOP, 1);

    for (int i = 0; i < 40; i++)
        stepSlot(&g_stream_b, HALF_B);

    CHECK(g_slots[0].strength == config.strength_max);
    CHECK(g_slots[0].frequency_ms == config.frequency_fast_ms);
}

// A finger held on the bottom rule asks for a waveform value of zero; the mode
// uploads nothing for it, which is the same "an all-zero batch does not leave the
// console" rule the motion mode's still periods use.
static void testHeldAtZeroIsSilent(void)
{
    DglabMotionFeedConfig config;
    bool any_strength = false;

    dglabMotionSettingsDefault(&config);
    config.frequency_follows_level = false;
    resetStreams(&config);

    resetFeed();
    holdAt(100, DGLAB_TOUCH_VALUE_BOTTOM, 1);

    CHECK(half(HALF_A)->held);

    for (int i = 0; i < 20; i++) {
        stepSlot(&g_stream_a, HALF_A);

        if (g_slots[0].strength > 0)
            any_strength = true;
    }

    CHECK(!any_strength);
    CHECK(!dglabMotionFeedIsStreaming(&g_stream_a));
}

int main(void)
{
    testRegionSplit();
    testValueAxis();
    testDensityAxis();
    testOneFingerPerHalfAtOnce();
    testSecondFingerTakesTheHalf();
    testLiftHandsTheHalfBack();
    testLiftReleasesTheHalf();
    testCrossingTheMiddle();
    testPositionsBecomeSlots();
    testDensityIsNotTheValue();
    testSharedParameters();
    testReleaseThenStop();
    testClampBandsReachTheEnds();
    testHeldAtZeroIsSilent();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
