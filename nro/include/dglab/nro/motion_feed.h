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

    // The density switch every mode shares (motion_settings.c owns it): while it
    // is on the pulse interval is `frequency_fixed_ms` and nothing else - the
    // motion mode's level and the touch mode's horizontal axis are both ignored,
    // so the density stops being a second thing to steer. While it is off each
    // mode keeps its own driver (see frequency_follows_level below).
    bool density_fixed;
    uint16_t frequency_fixed_ms; ///< the interval used while density_fixed

    uint8_t strength_max; ///< 0..100 waveform strength at full scale

    // Whether the pulse interval follows the level, which is what the motion mode
    // wants: a harder swing is also a denser one. A mode that names its own
    // targets (dglabMotionFeedSetTarget below - the touch mode, whose density is
    // the other axis of the panel) turns this off and gets exactly the interval it
    // asks for. The mode still decides this one - motion_settings.c neither reads
    // nor writes it - but it only has a say while density_fixed is off.
    bool frequency_follows_level;
} DglabMotionFeedConfig;

typedef struct {
    DglabMotionFeedConfig config;
    bool moving;        ///< currently above the dead zone (hysteresis applied)
    bool streaming;     ///< false while idle for longer than idle_stop_ms
    float level;        ///< 0..1 envelope, what the UI shows
    float window_peak;  ///< highest raw intensity in the current slot window
    float window_density; ///< pulse density asked for by SetTarget, 0..1
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

/// Hands the envelope a target directly, for a mode whose input is not an IMU:
/// `level` is the waveform value 0..1 this window wants and `density` the pulse
/// density 0..1 (0 = frequency_still, 1 = frequency_fast). Call it once per frame
/// while the input is there, and simply stop calling it when the input goes away:
/// the release curve, the idle stop and the "an all-zero batch is not uploaded"
/// rule are then the same ones the sensor path runs, which is the point of the
/// two modes sharing this feed.
void dglabMotionFeedSetTarget(DglabMotionFeed* feed, float level, float density);

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

// ---------------------------------------------------------------------------
// Sensor link state
//
// One side's sensor is reached through more than one HID handle (the pair style
// hands over one per side, and the single styles hand over another for the same
// physical controller), and each handle answers independently: it may have
// readings, it may have nothing, and a reading may be a placeholder that says
// "not connected". Turning that into the one "is this side connected?" the
// screen shows is a rule, and it is here rather than in joycon.c so the host
// tests can hold it down (docs/joycon-input.md).
// ---------------------------------------------------------------------------

/// What one side's handles reported during a poll.
typedef struct {
    bool answered;     ///< at least one handle had readings to hand over
    bool connected;    ///< at least one of those readings said IsConnected
    unsigned samples;  ///< readings that were handed to the feed
    // The console does not present this side as a usable detached Joy-Con: it is
    // attached to the console (handheld), switched off, or simply not there. The
    // six-axis handle is no help here - it keeps handing over readings for a
    // Joy-Con that is attached or switched off (hardware report, 2026-09-17),
    // which is why the rows never changed - so the caller asks the pad API and
    // says so. A side that is not usable is not polled at all.
    bool not_usable;
} DglabMotionSensorPoll;

/// How many polls in a row a side may deliver nothing before it counts as gone.
///
/// A connected Joy-Con fills the system's six-axis LIFO every frame - on
/// hardware, each poll handed over 16 readings - so silence is not idleness, it
/// is a controller that was taken off, turned off, or has fallen asleep. This is
/// the case the row used to miss: such a controller stops producing readings
/// instead of announcing that it left, and a rule that only believes what it is
/// told keeps showing the last state forever.
///
/// Twenty polls is a third of a second at 60Hz. Counting polls rather than
/// milliseconds is deliberate: a slow frame or a stalled IPC call shows up as
/// one poll, not twenty, so a hitch cannot look like a disconnect.
#define DGLAB_MOTION_SENSOR_QUIET_POLLS 20u

/// The side's connection state after a poll.
///
/// Readings are the only proof: a reading reaches the feed only when it said it
/// was connected, and a connected sensor fills the system's LIFO every frame. So
/// "output works but the screen says not connected" cannot happen, and - the
/// case that mattered on hardware - neither can a side that says "connected"
/// while handing over nothing: the state follows what actually arrived.
///
/// `quiet_polls` is how many polls in a row produced no readings at all. Below
/// DGLAB_MOTION_SENSOR_QUIET_POLLS it keeps `previous` (the first frames after
/// the mode starts have nothing to show yet); past it the side counts as
/// disconnected.
bool dglabMotionSensorConnected(bool previous, const DglabMotionSensorPoll* poll,
    unsigned quiet_polls);
