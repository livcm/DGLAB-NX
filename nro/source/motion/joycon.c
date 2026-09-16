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

bool dglabJoyconStart(void)
{
    HidSixAxisSensorHandle pair[2] = { 0 };
    HidSixAxisSensorHandle left = { 0 };
    HidSixAxisSensorHandle right = { 0 };
    bool any = false;

    if (dglabJoyconStarted())
        return true;

    memset(g_joycon, 0, sizeof(g_joycon));

    // Filled from the system; failures just mean that style is not present, so
    // nothing here is fatal.
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(pair, 2, HidNpadIdType_No1,
            HidNpadStyleTag_NpadJoyDual))) {
        addHandle(&g_joycon[DglabJoycon_Left], pair[0]);
        addHandle(&g_joycon[DglabJoycon_Right], pair[1]);
    }

    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&left, 1, HidNpadIdType_No1,
            HidNpadStyleTag_NpadJoyLeft)))
        addHandle(&g_joycon[DglabJoycon_Left], left);

    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&right, 1, HidNpadIdType_No1,
            HidNpadStyleTag_NpadJoyRight)))
        addHandle(&g_joycon[DglabJoycon_Right], right);

    for (size_t side = 0; side < 2; side++) {
        for (size_t i = 0; i < g_joycon[side].handle_count; i++) {
            if (R_SUCCEEDED(hidStartSixAxisSensor(g_joycon[side].handles[i]))) {
                g_joycon[side].started = true;
                any = true;
            }
        }
    }

    return any;
}

void dglabJoyconStop(void)
{
    for (size_t side = 0; side < 2; side++) {
        for (size_t i = 0; i < g_joycon[side].handle_count; i++) {
            if (g_joycon[side].started)
                hidStopSixAxisSensor(g_joycon[side].handles[i]);
        }

        g_joycon[side].started = false;
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

    written = snprintf(out, size, "%s: handles %u, quiet %u",
        side == DglabJoycon_Left ? "left" : "right", (unsigned)state->handle_count,
        state->quiet_polls);

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
