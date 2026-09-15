#pragma once

// Motion to waveform feed: turns Joy-Con motion into the 25ms waveform slots the
// sysmodule streams to the App (see docs/joycon-input.md).
//
// This file is deliberately platform independent - it never includes libnx and
// only uses C's standard headers - so the mapping can be tuned against
// tests/motion instead of against a console. nro/source/motion/joycon.c is the
// part that reads the sensor and hands samples in.
//
// Usage per frame:
//
//   for each sensor sample the frame produced
//       dglabMotionFeedAddSample(&feed, &sample);
//   count = dglabMotionFeedAdvance(&feed, elapsed_ns, slots, capacity);
//   if (count) upload slots - but only while dglabMotionFeedIsStreaming(&feed)

#include <dglab/ipc.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// One sensor reading. Units are whatever the platform reports (libnx hands over
/// floats; the order of magnitude is calibrated in the config below).
typedef struct {
    float angular_velocity[3];
    float acceleration[3];
    uint32_t delta_time_us; ///< since the previous sample, 0 when unknown
    bool interpolated;      ///< true when the platform made this up
} DglabMotionSample;

typedef struct {
    // Intensity = clamp01(w_gyro * |w| / w_ref + w_accel * |da| / a_ref).
    float gyro_weight;
    float gyro_reference;  ///< |angular velocity| that counts as full scale
    float accel_weight;
    float accel_reference; ///< |acceleration change| that counts as full scale

    // Holding a controller still is never perfectly still, so tiny readings are
    // dropped. Two thresholds: the level has to reach `deadzone_enter` to count
    // as movement, and has to fall below `deadzone_exit` to stop counting -
    // otherwise a value hovering on the edge chatters.
    float deadzone_enter;
    float deadzone_exit;

    // Envelope in milliseconds: quick to follow a swing, slow to let go.
    uint32_t attack_ms;
    uint32_t release_ms;

    // How long the feed stays silent before the producer should stop uploading,
    // so that a controller lying in a hand costs no traffic at all. The stream
    // only stops once the release has finished as well (see
    // dglabMotionFeedIsStreaming), otherwise the tail would be cut off.
    uint32_t idle_stop_ms;

    // Frequency follows the intensity: still at `frequency_still_ms`, fastest at
    // `frequency_fast_ms`. Both are app side milliseconds and must stay inside
    // 10..1000 (10..100 maps to the device value unchanged).
    uint16_t frequency_still_ms;
    uint16_t frequency_fast_ms;

    uint8_t strength_max; ///< 0..100 waveform strength at full scale
} DglabMotionFeedConfig;

typedef struct {
    DglabMotionFeedConfig config;
    bool moving;        ///< currently above the dead zone (hysteresis applied)
    bool streaming;     ///< false while idle for longer than idle_stop_ms
    float level;        ///< 0..1 envelope, what the UI shows
    float window_peak;  ///< highest raw intensity in the current slot window
    uint32_t window_ns; ///< time accumulated towards the next slot
    uint32_t idle_ms;   ///< time since the last window with any movement
    float last_acceleration[3];
    bool have_last_acceleration;
} DglabMotionFeed;

/// Fills in usable defaults; the caller may adjust fields before Init or after,
/// since the config is copied.
void dglabMotionFeedDefaultConfig(DglabMotionFeedConfig* config);

/// Copies the config and clears the running state (call it when the mode is
/// entered, so a previous session cannot leak into this one).
void dglabMotionFeedInit(DglabMotionFeed* feed, const DglabMotionFeedConfig* config);

/// Feeds one sensor sample. Interpolated samples are ignored on purpose.
void dglabMotionFeedAddSample(DglabMotionFeed* feed, const DglabMotionSample* sample);

/// Advances the clock and writes the slots that are due. Returns how many slots
/// were written (never more than `max`), which is 0 or 1 on most frames and
/// several after a stall - the stream stays paced by time, not by frame count.
size_t dglabMotionFeedAdvance(DglabMotionFeed* feed, uint32_t elapsed_ns,
    DglabNetWaveformSlot* slots, size_t max);

/// 0..1 envelope for the UI.
float dglabMotionFeedLevel(const DglabMotionFeed* feed);

/// The frequency that goes with the current level, in app side milliseconds.
uint16_t dglabMotionFeedFrequencyMs(const DglabMotionFeed* feed);

/// Whether the caller should upload at all: false once the controller has been
/// still for `idle_stop_ms` (the decay has already been delivered by then).
bool dglabMotionFeedIsStreaming(const DglabMotionFeed* feed);

/// Whether the dead zone currently reports movement.
bool dglabMotionFeedIsMoving(const DglabMotionFeed* feed);
