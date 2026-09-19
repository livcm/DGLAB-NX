#pragma once

// The touch panel as a screen works with it: the coordinates readings arrive in,
// and the two lines of a page that double as the axes of the touch mode (see
// docs/touch-input.md).
//
// Nothing here touches libnx. nro/source/platform/touch.c hands readings over in
// exactly this shape, and the mapping that turns them into waveform targets
// (dglab/nro/touch_feed.h) reads them back out - which is what lets the mapping,
// the region split and the axis ends be tested against tests/touch instead of
// against a console.

#include <stdbool.h>
#include <stdint.h>

/// The handheld panel's own coordinate system. It happens to be exactly the 720p
/// system every screen in this NRO is laid out in, so a reading needs no
/// conversion on its way to the canvas - which applies the display scale (3/2 on
/// a docked 1080p frame) itself, like it does for every other coordinate.
#define DGLAB_TOUCH_PANEL_WIDTH 1280
#define DGLAB_TOUCH_PANEL_HEIGHT 720

/// How many readings one snapshot can carry (libnx declares
/// HidTouchScreenState::touches[16]).
#define DGLAB_TOUCH_MAX_POINTS 16

/// Where the two halves meet: x < this is channel A, x >= it is channel B. It is
/// the middle of the panel, and the page draws its white centre line here.
#define DGLAB_TOUCH_SPLIT 640
/// The width of one half, in the panel's pixels. The rightmost column of a half
/// is left + DGLAB_TOUCH_HALF_WIDTH - 1, which is what a full density reading
/// comes from.
#define DGLAB_TOUCH_HALF_WIDTH 640

/// The value axis. Both y values are lines the page already draws - the rule
/// under the title bar and the one above the bottom bar - so the axes need no
/// extra chrome: reaching above the first one is full waveform value, reaching
/// below the second one is silence.
#define DGLAB_TOUCH_VALUE_TOP 87
#define DGLAB_TOUCH_VALUE_BOTTOM 647

/// One reading, in panel coordinates.
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t finger_id;
    /// Whether this finger went down / came up during this frame, gathered from
    /// every snapshot the frame drained rather than from the newest one alone:
    /// the panel sets those bits only on the snapshot the edge happened in, and
    /// a finger put down and held between two polls would otherwise look like a
    /// finger that had been there all along.
    bool start;
    bool end;
} DglabTouchPoint;

/// One frame's worth: the panel's newest snapshot, with the edges of the drained
/// snapshots merged into it (see nro/source/platform/touch.c).
typedef struct {
    unsigned count;
    DglabTouchPoint points[DGLAB_TOUCH_MAX_POINTS];
} DglabTouchFrame;

/// The channel a reading belongs to, from its x alone.
static inline bool dglabTouchIsLeft(uint32_t x)
{
    return x < DGLAB_TOUCH_SPLIT;
}

/// The left edge of the half the reading is in.
static inline uint32_t dglabTouchHalfLeft(uint32_t x)
{
    return dglabTouchIsLeft(x) ? 0u : (uint32_t)DGLAB_TOUCH_SPLIT;
}
