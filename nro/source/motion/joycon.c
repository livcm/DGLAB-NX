#include <dglab/nro/joycon.h>

#include <switch.h>

#include <stdio.h>
#include <string.h>

// The lid of samples the system keeps per sensor; asking for all of them means a
// frame that ran late still sees every reading instead of one.
#define DRAIN_MAX 17u

// Handles one side may hold: the pair style gives one per side, and the single
// style gives another for the same physical controller.
#define HANDLES_MAX 2u

// A side that has produced nothing for this many polls may be holding handles
// from an assignment that no longer exists: the console went handheld, or the
// controller was re-synced and came back under a different style. Taking a fresh
// set is what the design already promises ("however the system hands the
// controllers over, the side is readable"), and it is the difference between a
// side that says "not connected" for a moment and one that says it for the rest
// of the session while its channel stays silent.
#define RESCAN_AFTER_QUIET_POLLS 120u

// A pair of Joy-Cons is exposed as one "JoyDual" handle per side, and a lone one
// as a JoyLeft/JoyRight handle. A side may answer on either, depending on how
// the system assigned the controllers, so both are held and tried in order.
//
// The order matters: the pair handle comes first and the single style handle is
// only a fallback, because both are the same physical controller. Polling both
// would hand the same reading to the feed twice - which halves the acceleration
// delta term, since a duplicate looks like "no change" - and, worse, an inactive
// style answers with placeholders that used to decide the whole side's
// connection state (see docs/joycon-input.md).
typedef struct {
    HidSixAxisSensorHandle handles[HANDLES_MAX];
    size_t handle_count;
    bool started;
    bool connected;
    // Polls in a row that produced no readings at all: a live sensor fills the
    // LIFO every frame, so a run of these is how a controller that was turned
    // off or plugged back in shows up (see dglabMotionSensorConnected).
    unsigned quiet_polls;
    // How often this side gave up on its handles and took a fresh set.
    unsigned rescans;
    // What the last poll asked each handle and what it answered, oldest first,
    // for the log line dglabJoyconDescribe() builds. A handle the poll did not
    // reach (see the order rule below) keeps polled=false, which the log says
    // out loud: "no readings" and "not asked" are different answers.
    struct {
        bool polled;
        size_t states;
        size_t samples;
        bool connected;
    } last[HANDLES_MAX];
} JoyconState;

static JoyconState g_joycon[2];

static void addHandle(JoyconState* state, HidSixAxisSensorHandle handle)
{
    if (state->handle_count < sizeof(state->handles) / sizeof(state->handles[0]))
        state->handles[state->handle_count++] = handle;
}

// Stops whatever this side has running and forgets the handles. The connection
// state is left alone: it is the caller's, and a rescan must not make the row
// jump.
static void stopSide(JoyconState* state)
{
    for (size_t i = 0; i < state->handle_count; i++) {
        if (state->started)
            hidStopSixAxisSensor(state->handles[i]);
    }

    state->handle_count = 0;
    state->started = false;
    state->quiet_polls = 0;
}

// The handles one side may answer on, in the order they are tried, started.
static bool acquireSide(JoyconState* state, DglabJoyconSide side)
{
    HidSixAxisSensorHandle pair[2] = { 0 };
    HidSixAxisSensorHandle single = { 0 };
    HidNpadStyleTag single_style = side == DglabJoycon_Left ? HidNpadStyleTag_NpadJoyLeft
                                                            : HidNpadStyleTag_NpadJoyRight;
    bool any = false;

    stopSide(state);

    // Filled from the system; failures just mean that style is not present, so
    // nothing here is fatal.
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(pair, 2, HidNpadIdType_No1,
            HidNpadStyleTag_NpadJoyDual)))
        addHandle(state, pair[side == DglabJoycon_Left ? 0 : 1]);

    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&single, 1, HidNpadIdType_No1, single_style)))
        addHandle(state, single);

    for (size_t i = 0; i < state->handle_count; i++) {
        if (R_SUCCEEDED(hidStartSixAxisSensor(state->handles[i]))) {
            state->started = true;
            any = true;
        }
    }

    return any;
}

bool dglabJoyconStart(void)
{
    bool any = false;

    // A fresh set every time the mode is entered: handles from an earlier visit
    // may belong to an assignment the system has changed since.
    for (size_t side = 0; side <= (size_t)DglabJoycon_Right; side++) {
        g_joycon[side].connected = false;
        g_joycon[side].rescans = 0;

        if (acquireSide(&g_joycon[side], (DglabJoyconSide)side))
            any = true;
    }

    return any;
}

void dglabJoyconStop(void)
{
    for (size_t side = 0; side <= (size_t)DglabJoycon_Right; side++) {
        stopSide(&g_joycon[side]);
        g_joycon[side].connected = false;
    }
}

bool dglabJoyconStarted(void)
{
    return g_joycon[DglabJoycon_Left].started || g_joycon[DglabJoycon_Right].started;
}

// What every handle of one side adds up to. Filled in as the handles are
// polled; the connection state is decided from it afterwards.
typedef struct {
    DglabMotionSensorPoll poll;
    size_t states;
} PollTotals;

// Polls one handle, appending to `totals` and to the sample array, and reports
// what this handle alone had.
static void pollHandle(HidSixAxisSensorHandle handle, DglabMotionSample* out, size_t max,
    PollTotals* totals, size_t* out_states, size_t* out_samples, bool* out_connected)
{
    HidSixAxisSensorState states[DRAIN_MAX];
    size_t count = hidGetSixAxisSensorStates(handle, states, DRAIN_MAX);
    size_t samples_before = totals->poll.samples;
    bool connected = false;

    if (count == 0) {
        *out_states = 0;
        *out_samples = 0;
        *out_connected = false;

        return;
    }

    for (size_t i = 0; i < count && totals->poll.samples < max; i++) {
        const HidSixAxisSensorState* state = &states[i];

        // A reading that is not connected is a placeholder for a controller that
        // is away or asleep, and an interpolated one was made up by the system:
        // neither is movement. A placeholder does not undo what another handle
        // (or an earlier reading of this one) said - that is what used to leave
        // the row on "not connected" while the waveform was already playing.
        if (!(state->attributes & HidSixAxisSensorAttribute_IsConnected)) {
            continue;
        }

        connected = true;

        if (state->attributes & HidSixAxisSensorAttribute_IsInterpolated)
            continue;

        memcpy(out[totals->poll.samples].angular_velocity, &state->angular_velocity,
            sizeof(out[totals->poll.samples].angular_velocity));
        memcpy(out[totals->poll.samples].acceleration, &state->acceleration,
            sizeof(out[totals->poll.samples].acceleration));
        out[totals->poll.samples].delta_time_us = (uint32_t)(state->delta_time / 1000ull);
        out[totals->poll.samples].interpolated = false;

        totals->poll.samples++;
    }

    // Everything this handle reported, for the side's totals and the log line.
    totals->poll.answered = true;
    totals->states += count;
    totals->poll.connected = totals->poll.connected || connected;

    *out_states = count;
    *out_samples = totals->poll.samples - samples_before;
    *out_connected = connected;
}

size_t dglabJoyconPoll(DglabJoyconSide side, DglabMotionSample* out, size_t max)
{
    PollTotals totals;
    JoyconState* state;

    if (!out || max == 0 || side > DglabJoycon_Right)
        return 0;

    state = &g_joycon[side];
    memset(&totals, 0, sizeof(totals));

    for (size_t i = 0; i < state->handle_count; i++)
        memset(&state->last[i], 0, sizeof(state->last[i]));

    for (size_t i = 0; i < state->handle_count; i++) {
        size_t states = 0;
        size_t samples = 0;
        bool connected = false;

        pollHandle(state->handles[i], out, max, &totals, &states, &samples, &connected);

        state->last[i].polled = true;
        state->last[i].states = states;
        state->last[i].samples = samples;
        state->last[i].connected = connected;

        // The handles of one side describe the same physical controller, so the
        // first one that really delivered something is the one to use.
        if (totals.poll.samples > 0)
            break;
    }

    if (totals.poll.samples > 0)
        state->quiet_polls = 0;
    else
        state->quiet_polls++;

    // Long silence may mean the handles themselves are stale rather than the
    // controller being gone (see RESCAN_AFTER_QUIET_POLLS): take a fresh set and
    // let the next polls decide. The count is in the log line, so a hardware run
    // shows whether this was needed.
    if (state->quiet_polls >= RESCAN_AFTER_QUIET_POLLS) {
        acquireSide(state, side);
        state->rescans++;
        state->quiet_polls = 0;
    }

    state->connected = dglabMotionSensorConnected(state->connected, &totals.poll,
        state->quiet_polls);

    return totals.poll.samples;
}

bool dglabJoyconIsConnected(DglabJoyconSide side)
{
    if (side > DglabJoycon_Right)
        return false;

    return g_joycon[side].connected;
}

size_t dglabJoyconDescribe(DglabJoyconSide side, char* out, size_t size)
{
    const JoyconState* state;
    int written;
    size_t used;

    if (!out || size == 0 || side > DglabJoycon_Right)
        return 0;

    out[0] = '\0';
    state = &g_joycon[side];

    written = snprintf(out, size, "%s: handles %u, quiet %u, rescans %u",
        side == DglabJoycon_Left ? "left" : "right", (unsigned)state->handle_count,
        state->quiet_polls, state->rescans);

    if (written <= 0 || (size_t)written >= size)
        return 0;

    used = (size_t)written;

    for (size_t i = 0; i < state->handle_count; i++) {
        if (!state->last[i].polled)
            written = snprintf(out + used, size - used, ", #%u not polled", (unsigned)i);
        else
            written = snprintf(out + used, size - used,
                ", #%u states %u, samples %u, connected %u", (unsigned)i,
                (unsigned)state->last[i].states, (unsigned)state->last[i].samples,
                state->last[i].connected ? 1u : 0u);

        if (written <= 0 || (size_t)written >= size - used)
            return used;

        used += (size_t)written;
    }

    return used;
}

size_t dglabJoyconStyleText(char* out, size_t size)
{
    static const struct {
        u32 bit;
        const char* name;
    } kStyles[] = {
        { HidNpadStyleTag_NpadFullKey, "fullkey" },
        { HidNpadStyleTag_NpadHandheld, "handheld" },
        { HidNpadStyleTag_NpadJoyDual, "joydual" },
        { HidNpadStyleTag_NpadJoyLeft, "joyleft" },
        { HidNpadStyleTag_NpadJoyRight, "joyright" },
    };
    u32 styles;
    int written;
    size_t used;

    if (!out || size == 0)
        return 0;

    styles = hidGetNpadStyleSet(HidNpadIdType_No1);

    written = snprintf(out, size, "0x%08X", (unsigned)styles);

    if (written <= 0 || (size_t)written >= size)
        return 0;

    used = (size_t)written;

    for (size_t i = 0; i < sizeof(kStyles) / sizeof(kStyles[0]); i++) {
        if (!(styles & kStyles[i].bit))
            continue;

        written = snprintf(out + used, size - used, "+%s", kStyles[i].name);

        if (written <= 0 || (size_t)written >= size - used)
            return used;

        used += (size_t)written;
    }

    return used;
}
