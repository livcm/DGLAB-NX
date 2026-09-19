#include <dglab/nro/touch_feed.h>

#include <string.h>

// The mode's whole mapping, and the one place it is written down:
//
//   y = 647 (the rule above the bottom bar) -> waveform value 0
//   y =  87 (the rule under the title bar)  -> waveform value 100
//   x = the left end of a half's axis       -> the sparsest pulses (freq still)
//   x = the right end of a half's axis      -> the densest ones (freq fast)
//
// The halves are split at the middle of the panel: x < 640 is A, x >= 640 is B.
// The horizontal axis does *not* run edge to edge: its outer ends are the ends of
// the two white rules and its inner ends are that middle, because a fingertip
// cannot reach the panel's own edge - an axis drawn to the edge never reached the
// frequency parameters at all (docs/touch-input.md, hardware report 2026-09-19).
// Outside an axis the reading is clamped, so pushing past the end still gets the
// extreme value instead of something near it.
//
// Nothing here reads a clock or a sensor, which is what makes every one of those
// sentences a line in tests/touch.

static float clamp01(float value)
{
    if (value < 0.0f)
        return 0.0f;

    if (value > 1.0f)
        return 1.0f;

    return value;
}

static int channelOfX(uint32_t x)
{
    return dglabTouchIsLeft(x) ? DglabTouchChannelValue_A : DglabTouchChannelValue_B;
}

float dglabTouchLevelForY(uint32_t y)
{
    float span = (float)(DGLAB_TOUCH_VALUE_BOTTOM - DGLAB_TOUCH_VALUE_TOP);

    return clamp01(((float)DGLAB_TOUCH_VALUE_BOTTOM - (float)y) / span);
}

float dglabTouchDensityForX(uint32_t x)
{
    // Each half reaches the full density range on its own: its own last column is
    // the densest pulse the frequency parameters allow, whether that column is
    // 639 (next to the centre line) or 1255 (next to the right rule's end).
    float offset = (float)x - (float)dglabTouchDensityStart(x);
    float span = (float)(dglabTouchDensityEnd(x) - dglabTouchDensityStart(x));

    return clamp01(offset / span);
}

// Which reading owns a half this frame.
//
// A finger that says it went down during this frame takes the half - the last
// such reading when two of them did, because the panel does not timestamp them
// and the frame's own order is the only order there is. Otherwise the finger that
// already owned the half keeps it while it is still down, and if it is gone and
// somebody else is in the half, that one takes over: a half is never left
// following a finger that has already lifted.
static const DglabTouchPoint* pickOwner(const DglabTouchFrame* frame, int channel,
    bool have_current, uint32_t current_id)
{
    const DglabTouchPoint* started = NULL;
    const DglabTouchPoint* current = NULL;
    const DglabTouchPoint* any = NULL;

    for (unsigned i = 0; i < frame->count; i++) {
        const DglabTouchPoint* point = &frame->points[i];

        if (point->end || channelOfX(point->x) != channel)
            continue;

        if (have_current && point->finger_id == current_id)
            current = point;

        if (point->start)
            started = point;

        any = point;
    }

    if (started)
        return started;

    if (current)
        return current;

    return any;
}

void dglabTouchFeedInit(DglabTouchFeed* feed)
{
    if (!feed)
        return;

    memset(feed, 0, sizeof(*feed));
}

void dglabTouchFeedUpdate(DglabTouchFeed* feed, const DglabTouchFrame* frame)
{
    if (!feed)
        return;

    for (unsigned channel = 0; channel < (unsigned)DglabTouchChannelValue_Count; channel++) {
        const DglabTouchPoint* owner = frame ? pickOwner(frame, (int)channel,
            feed->have_finger[channel], feed->finger_id[channel]) : NULL;
        DglabTouchChannelState* state = &feed->channel[channel];

        if (owner == NULL) {
            feed->have_finger[channel] = false;
            state->held = false;
            continue;
        }

        feed->have_finger[channel] = true;
        feed->finger_id[channel] = owner->finger_id;
        state->held = true;
        state->x = owner->x;
        state->y = owner->y;
        state->level = dglabTouchLevelForY(owner->y);
        state->density = dglabTouchDensityForX(owner->x);
    }
}

const DglabTouchChannelState* dglabTouchFeedChannel(const DglabTouchFeed* feed,
    unsigned channel)
{
    static const DglabTouchChannelState empty;

    if (!feed || channel >= (unsigned)DglabTouchChannelValue_Count)
        return &empty;

    return &feed->channel[channel];
}
