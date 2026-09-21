#pragma once

#include <switch/sf/service.h>

// Console view for the BLE transport PoC inside the sysmodule.
//
// The route is shelved (docs/ble-poc.md) and the view is a diagnostic tool: it
// drives the temporary POC IPC commands and mirrors the sysmodule log to
// sdmc:/switch/DGLAB-NX. It takes over the screen and the console, so the caller
// has to release the framebuffer first and re-create it afterwards.
//
// `dglab` is the caller's already open `dglab` session and stays owned by the
// caller: the sysmodule registers itself with max_sessions=1, so opening a
// second session here fails with 0x615 ("not found") even while the sysmodule
// is running fine.
void dglabBlePocViewRun(Service* dglab);
