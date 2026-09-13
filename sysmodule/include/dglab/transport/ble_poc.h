#pragma once

#include <switch.h>

#include <dglab/ipc_poc.h>

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
