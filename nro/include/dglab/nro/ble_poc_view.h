#pragma once

// Console view for the BLE transport PoC inside the sysmodule.
//
// The route is shelved (docs/ble-poc.md) and the view is a diagnostic tool: it
// drives the temporary POC IPC commands and mirrors the sysmodule log to
// sdmc:/switch/DGLAB-NX. It takes over the screen and the console, so the caller
// has to release the framebuffer first and re-create it afterwards.
void dglabBlePocViewRun(void);
