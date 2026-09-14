#pragma once

// Wi-Fi + WebSocket transport for the DG-LAB Socket protocol.
//
// Owns the listening socket, one thread per client connection, a timer thread
// driving the heartbeat, and the mutex that makes the platform independent
// server core (dglab/net/net_server.h) safe to call from the IPC thread and the
// network threads at the same time.
//
// The Switch side only needs the server half of WebSocket: the phone App is the
// client that scans the QR code.

#include <dglab/ipc.h>

#include <stddef.h>
#include <switch.h>

// Prepares the transport lock and the session core (random source, controller
// id, log ring). Must be called once at startup, before any other function here,
// including the status readers of the IPC handlers.
//
// Nothing else happens at startup on purpose: an earlier revision registered a
// power state module and spawned a thread here, and the console stopped booting
// at the logo. Threads, sockets and the power state watch all belong to
// dglabNetSocketStart().
void dglabNetSocketInitialize(void);

// Starts the server. Idempotent: calling it while the server already runs
// reports success. 0 selects DGLAB_NET_DEFAULT_PORT.
Result dglabNetSocketStart(u16 port);

// Stops the server and releases every connection.
Result dglabNetSocketStop(void);

Result dglabNetSocketGetStatus(DglabNetStatus* out);

// Writes the QR payload. Fails while the Switch has no LAN address, which is
// the normal state before the console joined a network.
Result dglabNetSocketGetQr(char* out, size_t out_size, size_t* out_written);

Result dglabNetSocketSend(const DglabNetSendRequest* request);

// Reads the log ring. Returns the cursor to pass in next time.
u32 dglabNetSocketReadLog(u32 cursor, char* out, size_t out_size);
