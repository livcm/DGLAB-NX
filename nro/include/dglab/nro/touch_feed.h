#pragma once

// Touch panel readings -> waveform targets (see docs/touch-input.md).
//
// Platform independent on purpose (no libnx): nro/source/platform/touchscreen.c
// hands a frame of readings over, and everything decided here - which half a
// finger belongs to, which finger owns a half when several are down, and what a
// position means for the value and the density - is held down by tests/touch on a
// PC rather than by a session on a console.
//
// The mode then drives the same 25ms slot producer the motion mode uses
// (dglab/nro/motion_feed.h): its envelope, its idle stop and the parameter file
// both modes read are one implementation, not two.

#include <dglab/nro/touch_panel.h>

#include <stdbool.h>
#include <stdint.h>

/// The two halves, in the order the frame loop walks them. A is the left half of
/// the panel, B the right one; the DG-LAB channels they drive are 1 and 2
/// (main.c), which is a different number and stays a different number.
typedef enum {
    DglabTouchChannelValue_A = 0,
    DglabTouchChannelValue_B,
    DglabTouchChannelValue_Count,
} DglabTouchChannelValue;

/// What one half asks for right now.
typedef struct {
    bool held;      ///< a finger is in this half right now
    float level;    ///< 0..1 waveform value, from the vertical axis
    float density;  ///< 0..1 pulse density, from the horizontal one
    uint32_t x;     ///< where the finger is, in panel coordinates
    uint32_t y;
} DglabTouchChannelState;

typedef struct {
    // Which finger owns each half. The panel reports a set of touches per
    // snapshot rather than a stream of events, so "this one is mine" is state
    // that has to survive between frames - and it is the reason a second finger
    // can take a half over without the first one's lift releasing it.
    bool have_finger[DglabTouchChannelValue_Count];
    uint32_t finger_id[DglabTouchChannelValue_Count];
    DglabTouchChannelState channel[DglabTouchChannelValue_Count];
} DglabTouchFeed;

/// Clears the state. Call it when the mode is entered: a finger from a previous
/// session must not survive into this one.
void dglabTouchFeedInit(DglabTouchFeed* feed);

/// Applies one frame of panel readings.
///
/// A half with no finger keeps its last level and density and only drops `held`:
/// the release curve is the slot producer's job, and a density that stays where
/// the finger left it is what keeps the pulse interval steady while the value
/// fades out.
void dglabTouchFeedUpdate(DglabTouchFeed* feed, const DglabTouchFrame* frame);

/// What this half asks for right now. Never NULL for a valid channel.
const DglabTouchChannelState* dglabTouchFeedChannel(const DglabTouchFeed* feed,
    unsigned channel);

/// The two axes on their own, without any of the finger bookkeeping: the value a
/// y coordinate means, and the density an x coordinate means. Both are clamped,
/// so a finger on the page header is full value rather than nothing - the two
/// white rules are the ends of the axis (docs/touch-input.md).
float dglabTouchLevelForY(uint32_t y);
float dglabTouchDensityForX(uint32_t x);
