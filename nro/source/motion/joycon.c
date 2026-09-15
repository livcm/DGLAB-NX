#include <dglab/nro/joycon.h>

#include <switch.h>

#include <string.h>

// The lid of samples the system keeps per sensor; asking for all of them means a
// frame that ran late still sees every reading instead of one.
#define DRAIN_MAX 17u

// A pair of Joy-Cons is exposed as one "JoyDual" handle per side, and a lone one
// as a JoyLeft/JoyRight handle. A side may answer on either, depending on how
// the system assigned the controllers, so both are held and tried in order.
typedef struct {
    HidSixAxisSensorHandle handles[2];
    size_t handle_count;
    bool started;
    bool connected;
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

static bool pollHandle(HidSixAxisSensorHandle handle, DglabMotionSample* out, size_t max,
    size_t* written, bool* connected)
{
    HidSixAxisSensorState states[DRAIN_MAX];
    size_t count = hidGetSixAxisSensorStates(handle, states, DRAIN_MAX);

    if (count == 0)
        return false;

    for (size_t i = 0; i < count && *written < max; i++) {
        const HidSixAxisSensorState* state = &states[i];

        // A reading that is not connected is a placeholder, and an interpolated
        // one was made up by the system: neither is movement.
        if (!(state->attributes & HidSixAxisSensorAttribute_IsConnected)) {
            *connected = false;
            continue;
        }

        *connected = true;

        if (state->attributes & HidSixAxisSensorAttribute_IsInterpolated)
            continue;

        memcpy(out[*written].angular_velocity, &state->angular_velocity,
            sizeof(out[*written].angular_velocity));
        memcpy(out[*written].acceleration, &state->acceleration,
            sizeof(out[*written].acceleration));
        out[*written].delta_time_us = (uint32_t)(state->delta_time / 1000ull);
        out[*written].interpolated = false;

        (*written)++;
    }

    return true;
}

size_t dglabJoyconPoll(DglabJoyconSide side, DglabMotionSample* out, size_t max)
{
    size_t written = 0;
    bool connected = false;
    bool answered = false;
    JoyconState* state;

    if (!out || max == 0 || side > DglabJoycon_Right)
        return 0;

    state = &g_joycon[side];

    for (size_t i = 0; i < state->handle_count; i++) {
        if (pollHandle(state->handles[i], out, max, &written, &connected))
            answered = true;
    }

    if (answered || written > 0)
        state->connected = connected;

    return written;
}

bool dglabJoyconIsConnected(DglabJoyconSide side)
{
    if (side > DglabJoycon_Right)
        return false;

    return g_joycon[side].connected;
}
