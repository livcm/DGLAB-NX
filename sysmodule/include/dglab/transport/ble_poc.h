#pragma once

#include <switch.h>

#include <dglab/ipc_poc.h>
#include <dglab/ipc.h>

// BLE transport proof of concept.
//
// This is the only place in the project that talks to the Switch Bluetooth
// stack. It runs a worker thread that scans for the Coyote 3.0, connects,
// resolves the GATT service and characteristics from the official protocol
// documentation, subscribes to notifications and writes B0 packets, reporting
// everything through a log ring buffer that the NRO reads over IPC.
//
// It is deliberately separate from the protocol layer: packet construction uses
// dglab/protocol/coyote_v3.h, and this file only moves bytes.

// Initialises the PoC state. Call once before serving IPC requests.
void blePocInitialize(void);

// Starts a PoC run. The worker thread is created on demand, so nothing touches
// the Bluetooth stack at boot.
Result blePocStart(const DglabPocStartRequest* request);

// Requests a stop. Returns immediately; the worker cleans up on its own.
Result blePocStop(void);

// Queues an action (DglabPocAction_*) for the worker.
Result blePocAction(const DglabPocActionRequest* request);

// Copies the current status.
void blePocGetStatus(DglabPocStatus* out);

// Copies up to out_size-1 bytes of log text starting at cursor and returns the
// next cursor. Cursors are absolute byte offsets into an internal ring buffer,
// so a reader that falls behind simply skips forward.
u32 blePocReadLog(u32 cursor, char* out, u32 out_size);

// ---------------------------------------------------------------------------
// The BLE session (the mode a client uses to actually play)
// ---------------------------------------------------------------------------
//
// The probes above stay as they are. This is the transport a client drives the
// device through: it takes the same two inputs the Socket mode takes
// (DGLAB_IPC_CMD_NET_SEND for strength, DGLAB_IPC_CMD_NET_WAVEFORM for
// waveform data) and writes them to the device through the local Coyote V3
// session. There is no readback on this firmware (docs/ble-re.md, "连接所有权在
// 服务层是封的"), so the status reports what this side asked for.

// Starts a session with the given soft limit (0..200; 0 means the device cannot
// output anything). The worker thread runs the usual bring-up - the caller has
// to have run the driver-level probe in an earlier session of this boot, see
// docs/ble-poc.md - then connects, subscribes and streams until stopped.
Result blePocSessionStart(const DglabBleStartRequest* request);

// Requests a stop; the worker caps the device and disconnects on its own.
Result blePocSessionStop(void);

// Copies the session status. Open loop: strength_a/strength_b are the values
// this side last asked for.
void blePocSessionGetStatus(DglabBleStatus* out);

// True while a session is requested or running, so a client can route its
// gameplay inputs (or the sysmodule can, see main.c).
bool blePocSessionIsActive(void);

// Queues waveform data for the session (no-op when none is running).
Result blePocSessionUploadWaveform(const DglabNetWaveformRequest* request);

// Queues a Socket V3 message for the session: `strength-<ch>+<mode>+<value>`
// is what the gameplay sends (docs/dglab-socket.md), and it is applied to the
// local session instead of being forwarded to a phone.
Result blePocSessionSend(const DglabNetSendRequest* request);

// Moves the session's ceiling (0..200). The worker writes the new BF soft limit
// on its next step, so a running session follows immediately and strength the
// caller asks for afterwards is clamped to the new value. Rejected while no
// session is running - the start request is what sets it for a fresh session.
Result blePocSessionSetSoftLimit(u32 soft_limit);
