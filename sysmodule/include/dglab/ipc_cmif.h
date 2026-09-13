#pragma once

// CMIF request/reply layout rules used by the sysmodule IPC server.
//
// They live in a header so that the host side test in tests/ipc can check them
// against libnx's own request encoder instead of against a second copy of the
// same assumptions. The header only needs libnx's sf/cmif.h, which is
// header-only and compiles on a PC.
//
// Layout, as implemented by libnx (see cmifMakeRequest):
//   0x00  IPC header
//   ...   CMIF data area, aligned up to 16 bytes
//   +0x00 CmifInHeader / CmifOutHeader
//   +0x10 inline payload

#include <switch/sf/cmif.h>

// Alignment the CMIF data area starts at, relative to the IPC header.
#define DGLAB_CMIF_DATA_ALIGN 16u

// Size of the inline IPC area. The threads' IPC buffer is 0x100 bytes on the
// Switch, so a reply that exceeds this cannot be sent inline.
#define DGLAB_IPC_BUFFER_SIZE 0x100u

// The inline request payload follows the CmifInHeader inside the CMIF data area.
static inline const void* dglabRequestPayload(const CmifInHeader* in)
{
    return (const void*)(in + 1);
}

// Mirrors how libnx sizes a CMIF request: the data area starts at the IPC header
// aligned up to 16 bytes, followed by the CmifInHeader and the payload.
static inline bool dglabRequestHasPayload(u32 num_data_words, u32 size)
{
    return (u32)num_data_words * sizeof(u32) >=
           DGLAB_CMIF_DATA_ALIGN + (u32)sizeof(CmifInHeader) + size;
}

// Word count for a CMIF reply carrying data_size bytes inline. This is the same
// accounting libnx uses on the client side; the accounting allowance for the
// 16-byte alignment is what makes the payload reach the client intact.
static inline u32 dglabResponseDataWords(u32 data_size)
{
    u32 actual_size = DGLAB_CMIF_DATA_ALIGN + (u32)sizeof(CmifOutHeader) + data_size;

    actual_size = (actual_size + 1u) & ~1u;

    return (actual_size + 3u) / 4u;
}

// Whether a reply of the given payload size can be sent inline at all.
static inline bool dglabResponseFitsInline(u32 data_size)
{
    return dglabResponseDataWords(data_size) * sizeof(u32) <= DGLAB_IPC_BUFFER_SIZE;
}

// Conservative upper bound for an inline payload, usable in a static assertion.
#define DGLAB_IPC_INLINE_PAYLOAD_MAX \
    (DGLAB_IPC_BUFFER_SIZE - DGLAB_CMIF_DATA_ALIGN - (u32)sizeof(CmifOutHeader))
