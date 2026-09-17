#pragma once

// Keeps the console's automatic sleep timer away from the socket server.
//
// Holding sockets across a system sleep is what hangs the console, and the
// sysmodule cannot be warned that a sleep is coming: its psc registration is
// refused by the system (rc=0x0000108A, see docs/dglab-socket.md, "睡眠与唤醒").
// The front end has one thing the sysmodule has not - it is an applet - so while
// the socket server runs it can switch the automatic sleep off through
// ISelfController, and put it back when the server stops.
//
// This does not cover the power button: a sleep the user asks for still hangs the
// console. The socket page therefore keeps warning about sleeping, with the other
// wording while this suppression is in effect (DglabString_SleepWarningAutoOff).
//
// Nothing here may fail the server: every call answers with a Result, and the
// caller decides what to log.

#include <switch/types.h>

#include <stdbool.h>

/// What one call actually did, so the caller can log exactly one line for it
/// instead of guessing from a Result that is 0 for both "did it" and "did
/// nothing".
typedef enum {
    DglabAutoSleepEvent_None = 0,   ///< Nothing to do: the state already matched.
    DglabAutoSleepEvent_Suppressed, ///< Automatic sleep is off because of us.
    DglabAutoSleepEvent_AlreadyOff, ///< It was off before we asked; left alone.
    DglabAutoSleepEvent_Restored,   ///< Automatic sleep is back on.
    DglabAutoSleepEvent_Failed,     ///< An applet call failed, see \p out_rc.
} DglabAutoSleepEvent;

/// Follows the socket server state the caller just polled: \p running true turns
/// automatic sleep off, false turns it back on if this file is the one that
/// turned it off.
///
/// Only a change of \p running does anything - the callers poll every frame - and
/// a failed call is not retried until the server state changes again. \p out_rc
/// takes the Result of the applet call, or 0 when there was nothing to do.
DglabAutoSleepEvent dglabAutoSleepFollowServer(bool running, Result* out_rc);

/// True while automatic sleep is off and the socket server is why. The socket
/// page picks its warning line from this.
bool dglabAutoSleepActive(void);

/// Puts the console's automatic sleep flag back. A console that had it off
/// before the server started (the user's own setting, or another applet) keeps it
/// off: this file only undoes what it did itself. Called once on the way out of
/// the NRO.
DglabAutoSleepEvent dglabAutoSleepRestore(Result* out_rc);
