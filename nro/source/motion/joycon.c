#include <dglab/nro/joycon.h>

#include <switch.h>

#include <stdio.h>
#include <string.h>

// The lid of samples the system keeps per sensor; asking for all of them means a
// frame that ran late still sees every reading instead of one.
#define DRAIN_MAX 17u

// Handles one side may hold: normally the pair style's handle for that side, and
// the single style's one only on a console where the pair style is not available
// at all (see acquireAll).
#define HANDLES_MAX 2u

// How long *both* sides may produce nothing before the mode takes a fresh set of
// handles. One silent side is normal (that controller was plugged in or turned
// off) and must never trigger a re-acquire: doing that per side is how a side
// ended up bound to the other controller's handle, which showed up on hardware
// as one row's numbers moving to the other one and then freezing.
#define RESCAN_AFTER_QUIET_POLLS 120u

// A pair of Joy-Cons is exposed as one "JoyDual" handle per side, and a lone one
// as a JoyLeft/JoyRight handle. The pair handles are what this mode is built on
// and they are taken together, in one call, so the two sides cannot end up
// pointing at the same physical controller; the single style is only a fallback
// for a console where the pair style cannot be had at all.
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

// Takes one set of handles for both sides and starts them. The pair handles come
// from a single call, so pair[0] is one side and pair[1] the other by
// construction; the single style is only used when that call fails, i.e. on a
// console where the pair style is not available at all.
static int acquireAll(void)
{
    HidSixAxisSensorHandle pair[2] = { 0 };
    bool have_pair = R_SUCCEEDED(hidGetSixAxisSensorHandles(pair, 2, HidNpadIdType_No1,
        HidNpadStyleTag_NpadJoyDual));
    int started_sides = 0;

    for (size_t side = 0; side <= (size_t)DglabJoycon_Right; side++) {
        JoyconState* state = &g_joycon[side];
        HidNpadStyleTag single_style = side == (size_t)DglabJoycon_Left
            ? HidNpadStyleTag_NpadJoyLeft : HidNpadStyleTag_NpadJoyRight;
        HidSixAxisSensorHandle single = { 0 };
        bool started = false;

        stopSide(state);

        if (have_pair) {
            addHandle(state, pair[side == (size_t)DglabJoycon_Left ? 0 : 1]);
        } else if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&single, 1, HidNpadIdType_No1,
                       single_style))) {
            addHandle(state, single);
        }

        for (size_t i = 0; i < state->handle_count; i++) {
            if (R_SUCCEEDED(hidStartSixAxisSensor(state->handles[i]))) {
                state->started = true;
                started = true;
            }
        }

        if (started)
            started_sides++;
    }

    return started_sides;
}

bool dglabJoyconStart(void)
{
    // A fresh set every time the mode is entered (and that is the way to get one
    // after a Joy-Con was turned off or plugged back in: the row says "not
    // connected" until the mode is entered again).
    for (size_t side = 0; side <= (size_t)DglabJoycon_Right; side++) {
        g_joycon[side].connected = false;
        g_joycon[side].rescans = 0;
    }

    return acquireAll() > 0;
}

void dglabJoyconStop(void)
{
    for (size_t side = 0; side <= (size_t)DglabJoycon_Right; side++) {
        stopSide(&g_joycon[side]);
        g_joycon[side].connected = false;
    }
}

void dglabJoyconRescan(void)
{
    acquireAll();
    g_joycon[DglabJoycon_Left].rescans++;
    g_joycon[DglabJoycon_Right].rescans++;
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

size_t dglabJoyconPoll(DglabJoyconSide side, DglabMotionSample* out, size_t max, bool usable)
{
    PollTotals totals;
    JoyconState* state;

    if (!out || max == 0 || side > DglabJoycon_Right)
        return 0;

    state = &g_joycon[side];
    memset(&totals, 0, sizeof(totals));

    for (size_t i = 0; i < state->handle_count; i++)
        memset(&state->last[i], 0, sizeof(state->last[i]));

    // The caller has already asked the console (pad API) whether this side is a
    // usable detached Joy-Con. When it is not, the handles are left alone: they
    // would still report readings for an attached or switched-off Joy-Con, and
    // that is what kept the row alive (hardware report, 2026-09-17).
    if (!usable) {
        totals.poll.not_usable = true;
        state->connected = dglabMotionSensorConnected(state->connected, &totals.poll,
            state->quiet_polls);

        return 0;
    }

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

    // Both sides silent for a long time is the one case worth taking a fresh set
    // of handles: the mode was entered with the controllers off, or they were
    // re-synced since. One silent side is normal and must never re-acquire - that
    // is what let a side latch onto the other controller's handle.
    if (g_joycon[DglabJoycon_Left].quiet_polls >= RESCAN_AFTER_QUIET_POLLS &&
        g_joycon[DglabJoycon_Right].quiet_polls >= RESCAN_AFTER_QUIET_POLLS) {
        acquireAll();
        g_joycon[DglabJoycon_Left].rescans++;
        g_joycon[DglabJoycon_Right].rescans++;
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
