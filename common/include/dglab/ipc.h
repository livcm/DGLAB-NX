#pragma once

#include <switch/types.h>

// The service name is limited to 8 characters by the Switch service manager.
#define DGLAB_IPC_SERVICE_NAME "dglab"

// Minimal IPC protocol version. Increment whenever the IPC surface changes in a
// way that is not backward compatible.
#define DGLAB_IPC_PROTOCOL_VERSION 1u

// Commands implemented by the sysmodule. Command IDs are part of the public IPC
// contract and must not be renumbered once released.
enum {
    DGLAB_IPC_CMD_GET_VERSION = 0,
    DGLAB_IPC_CMD_PING        = 1,
};

// Value returned by DGLAB_IPC_CMD_PING. Keeping this stable gives clients a
// cheap way to verify that they are talking to the right service.
#define DGLAB_IPC_PING_MAGIC 0x44474C42u

// Version information returned by DGLAB_IPC_CMD_GET_VERSION.
typedef struct DglabIpcVersion {
    u32 major;
    u32 minor;
    u32 patch;
} DglabIpcVersion;

