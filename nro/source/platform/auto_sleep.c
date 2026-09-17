#include <dglab/nro/auto_sleep.h>

#include <switch.h>

// Three facts live here, and the second and third are not the same question:
//
//   - the last `running` the caller reported, so a per frame poll only acts on a
//     change, and a failure is not retried 60 times a second;
//   - whether automatic sleep is off right now, however it got that way. This is
//     what the socket page's warning line follows;
//   - whether *this* file is the one that turned it off. Automatic sleep is a
//     console-wide setting the user can change themselves (and other applets can
//     hold), so the NRO restores its own change and nobody else's.

static bool g_last_requested;
static bool g_have_requested;
static bool g_active;
static bool g_ours;

// Turns the flag on for the server. A flag the console reports as already set is
// not ours to clear later.
static DglabAutoSleepEvent setSuppressed(Result* out_rc)
{
    bool already = false;
    Result rc = appletIsAutoSleepDisabled(&already);

    *out_rc = rc;

    if (R_FAILED(rc))
        return DglabAutoSleepEvent_Failed;

    if (already) {
        g_active = true;
        g_ours = false;
        return DglabAutoSleepEvent_AlreadyOff;
    }

    rc = appletSetAutoSleepDisabled(true);
    *out_rc = rc;

    if (R_FAILED(rc))
        return DglabAutoSleepEvent_Failed;

    g_active = true;
    g_ours = true;

    return DglabAutoSleepEvent_Suppressed;
}

static DglabAutoSleepEvent setRestored(Result* out_rc)
{
    Result rc;

    if (!g_ours) {
        *out_rc = 0;
        g_active = false;
        return DglabAutoSleepEvent_None;
    }

    rc = appletSetAutoSleepDisabled(false);
    *out_rc = rc;

    if (R_FAILED(rc))
        return DglabAutoSleepEvent_Failed;

    g_active = false;
    g_ours = false;

    return DglabAutoSleepEvent_Restored;
}

DglabAutoSleepEvent dglabAutoSleepFollowServer(bool running, Result* out_rc)
{
    *out_rc = 0;

    if (g_have_requested && running == g_last_requested)
        return DglabAutoSleepEvent_None;

    g_last_requested = running;
    g_have_requested = true;

    if (running)
        return setSuppressed(out_rc);

    return setRestored(out_rc);
}

bool dglabAutoSleepActive(void)
{
    return g_active;
}

DglabAutoSleepEvent dglabAutoSleepRestore(Result* out_rc)
{
    // Whoever leaves the NRO with the server still running (docs/dglab-socket.md
    // calls that out as a known hole) has nothing to do with this flag: the
    // applet session is over either way, so this only ever undoes our own change.
    return setRestored(out_rc);
}
