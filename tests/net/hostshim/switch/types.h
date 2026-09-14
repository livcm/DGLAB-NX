#pragma once

// Host build shim for libnx's <switch/types.h>.
//
// common/include/dglab/ipc.h is the shared IPC contract, so it uses libnx's
// shorthand integer types. Pulling in devkitA64's real header tree on a PC would
// shadow the system's <arpa/inet.h> and break the socket based tests, so the
// host build resolves <switch/types.h> to this file instead.

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
