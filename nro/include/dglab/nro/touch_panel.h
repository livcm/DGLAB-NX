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

/// The density axis. Its ends are page geometry, not the edges of the panel: on
/// hardware a fingertip cannot get closer than about 20px to the edge, so an axis
/// that ran edge to edge left the pulse interval stuck between 32ms and 98ms
/// instead of reaching the parameters at all (docs/touch-input.md).
///
/// The outer ends are where the two white rules stop - x = DGLAB_PAGE_MARGIN and
/// one pixel short of the right margin, i.e. the first and last column of the
/// rules the page already draws. The page keeps them in step with the page frame
/// with a compile time assertion (nro/source/ui/touch.c); nothing in this header
/// includes a UI header, so the two numbers are repeated here on purpose.
#define DGLAB_TOUCH_DENSITY_LEFT 24
#define DGLAB_TOUCH_DENSITY_RIGHT 1255

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

/// The column a half's density axis starts at, and the one it ends at: the left
/// half runs from the left end of the rules to the middle of the panel, the right
/// half from the middle to the right end of the rules. Both spans are 615
/// columns, so both halves offer the same travel.
static inline uint32_t dglabTouchDensityStart(uint32_t x)
{
    return dglabTouchIsLeft(x) ? (uint32_t)DGLAB_TOUCH_DENSITY_LEFT : (uint32_t)DGLAB_TOUCH_SPLIT;
}

static inline uint32_t dglabTouchDensityEnd(uint32_t x)
{
    return dglabTouchIsLeft(x) ? (uint32_t)(DGLAB_TOUCH_SPLIT - 1)
                               : (uint32_t)DGLAB_TOUCH_DENSITY_RIGHT;
}
