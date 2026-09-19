#include <dglab/platform/touchscreen.h>

#include <switch.h>

#include <stdio.h>
#include <string.h>

// The LIFO depth libnx reads (HidTouchScreenLifo::storage[17]). Asking for all of
// it every frame is what keeps a tap that starts and ends between two frames
// from being missed - the same reason the six-axis side drains its handle.
#define TOUCH_LIFO_DEPTH 17

// How many readings dglabTouchDescribe() spells out. Enough to see a two finger
// touch, few enough to keep the line inside the log panel's 40 columns.
#define TOUCH_DESCRIBE_POINTS 3

static bool g_started;

// The drained snapshots. Static rather than a local: seventeen of them are 11 KB,
// and the poll only ever runs from the mode's frame loop.
static HidTouchScreenState g_states[TOUCH_LIFO_DEPTH];

// What the last poll saw, for dglabTouchDescribe(). Kept here instead of taken
// as an argument because the view writes its line where it decides to - once per
// visit and on every change - which is not the frame the poll happened in.
static size_t g_last_drained;
static DglabTouchFrame g_last_frame;
static bool g_last_valid;

void dglabTouchStart(void)
{
    if (g_started)
        return;

    // No Result to look at: libnx aborts to its own fatal error page when the
    // console refuses (see the header). Called once, when the mode is entered.
    hidInitializeTouchScreen();

    g_started = true;
}

// Whether any snapshot in this frame carried `attribute` for this finger. The
// newest snapshot alone is not enough: the panel sets Start and End only on the
// snapshot the edge happened in, and a frame can drain several of them.
static bool touchSawEdge(const HidTouchScreenState* states, size_t drained, uint32_t finger_id,
    u32 attribute)
{
    for (size_t state = 0; state < drained; state++) {
        for (s32 i = 0; i < states[state].count; i++) {
            if (states[state].touches[i].finger_id == finger_id &&
                (states[state].touches[i].attributes & attribute) != 0)
                return true;
        }
    }

    return false;
}

size_t dglabTouchPoll(DglabTouchFrame* frame)
{
    size_t drained;
    const HidTouchScreenState* newest;

    if (frame == NULL)
        return 0;

    memset(frame, 0, sizeof(*frame));

    drained = hidGetTouchScreenStates(g_states, TOUCH_LIFO_DEPTH);

    if (drained > 0) {
        // The newest snapshot is the last one libnx wrote, which is the order the
        // six-axis drain relies on too. Hardware is what confirms it for the
        // touch screen as well: the probe's log line prints the readings in the
        // order they came out.
        newest = &g_states[drained - 1];

        for (s32 i = 0; i < newest->count; i++) {
            const HidTouchState* touch;
            DglabTouchPoint* point;

            if (frame->count >= DGLAB_TOUCH_MAX_POINTS)
                break;

            touch = &newest->touches[i];
            point = &frame->points[frame->count++];

            point->x = touch->x;
            point->y = touch->y;
            point->finger_id = touch->finger_id;
            point->start = (touch->attributes & HidTouchAttribute_Start) != 0 ||
                touchSawEdge(g_states, drained, touch->finger_id, HidTouchAttribute_Start);
            point->end = (touch->attributes & HidTouchAttribute_End) != 0 ||
                touchSawEdge(g_states, drained, touch->finger_id, HidTouchAttribute_End);
        }
    }

    g_last_drained = drained;
    g_last_frame = *frame;
    g_last_valid = true;

    return drained;
}

size_t dglabTouchDescribe(char* out, size_t size)
{
    unsigned shown;
    int written;

    if (out == NULL || size == 0)
        return 0;

    if (!g_last_valid) {
        out[0] = '\0';
        return 0;
    }

    written = snprintf(out, size, "touch lifo %u, count %u", (unsigned)g_last_drained,
        g_last_frame.count);

    if (written <= 0 || (size_t)written >= size)
        return strlen(out);

    shown = g_last_frame.count;

    if (shown > TOUCH_DESCRIBE_POINTS)
        shown = TOUCH_DESCRIBE_POINTS;

    for (unsigned i = 0; i < shown; i++) {
        const DglabTouchPoint* point = &g_last_frame.points[i];
        int result = snprintf(out + written, size - (size_t)written,
            " | #%u x=%u y=%u id=%u%s%s", i, point->x, point->y, point->finger_id,
            point->start ? " start" : "", point->end ? " end" : "");

        if (result <= 0 || (size_t)result >= size - (size_t)written)
            break;

        written += result;
    }

    // The trailing state is only interesting when it is not the plain one.
    if (g_last_frame.count == 0 && g_last_drained == 0)
        snprintf(out + written, size - (size_t)written, " | idle");

    return strlen(out);
}

bool dglabTouchHandheld(void)
{
    return appletGetOperationMode() == AppletOperationMode_Handheld;
}
