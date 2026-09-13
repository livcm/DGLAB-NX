#include <stdio.h>
#include <string.h>

#include <switch.h>

#include <dglab/ipc.h>

#define INNER_HEAP_SIZE 0x80000

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit(void)
{
    Result rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    // libnx relies on the HOS version reported by set:sys for several service
    // behaviors (including sm serialization), so set it explicitly like the
    // devkitPro sysmodule template does.
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
}

void __appExit(void)
{
    smExit();
}

#ifdef __cplusplus
}
#endif

static Result dglabMakeResponse(u32 type, u32 token, Result result, const void* data, u32 data_size)
{
    void* tls = armGetTls();

    // The CMIF data area starts at the IPC header aligned up to 16 bytes, so the
    // word count has to include that alignment allowance. Without it the kernel
    // copies only the CmifOutHeader and truncates the payload, which makes
    // clients read zeroed data. This mirrors libnx's cmifMakeRequest accounting.
    u32 actual_size = 16u + (u32)sizeof(CmifOutHeader) + data_size;
    actual_size = (actual_size + 1u) & ~1u;
    u32 num_data_words = (actual_size + 3u) / 4u;

    HipcRequest hipc = hipcMakeRequestInline(tls,
        .type           = type,
        .num_data_words = num_data_words,
    );

    CmifOutHeader* out = (CmifOutHeader*)cmifGetAlignedDataStart(hipc.data_words, tls);
    out->magic   = CMIF_OUT_HEADER_MAGIC;
    out->version = 0;
    out->result  = result;
    out->token   = token;

    if (data_size)
        memcpy(out + 1, data, data_size);

    return 0;
}

static Result dglabMakeCloseResponse(void)
{
    hipcMakeRequestInline(armGetTls(),
        .type           = CmifCommandType_Close,
        .num_data_words = 0,
    );
    return 0;
}

static bool dglabHandleRequest(void)
{
    void* tls = armGetTls();
    HipcParsedRequest parsed = hipcParseRequest(tls);

    if (parsed.meta.type == CmifCommandType_Close) {
        dglabMakeCloseResponse();
        return true;
    }

    if (parsed.meta.type != CmifCommandType_Control &&
        parsed.meta.type != CmifCommandType_Request &&
        parsed.meta.type != CmifCommandType_RequestWithContext) {
        dglabMakeResponse(CmifCommandType_Request, 0,
            MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen), NULL, 0);
        return false;
    }

    if (parsed.meta.num_data_words < (sizeof(CmifInHeader) / sizeof(u32))) {
        dglabMakeResponse(CmifCommandType_Request, 0,
            MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen), NULL, 0);
        return false;
    }

    CmifInHeader* in = (CmifInHeader*)cmifGetAlignedDataStart(parsed.data.data_words, tls);
    if (in->magic != CMIF_IN_HEADER_MAGIC) {
        dglabMakeResponse(CmifCommandType_Request, 0,
            MAKERESULT(Module_Libnx, LibnxError_InvalidCmifOutHeader), NULL, 0);
        return false;
    }

    u32 command_id = in->command_id;
    u32 token = in->token;

    if (parsed.meta.type == CmifCommandType_Control) {
        if (command_id == 3) {
            // cmifQueryPointerBufferSize: this minimal service does not use
            // pointer buffers, so report a size of zero.
            u16 pointer_buffer_size = 0;
            dglabMakeResponse(CmifCommandType_Control, token, 0,
                &pointer_buffer_size, sizeof(pointer_buffer_size));
        } else {
            dglabMakeResponse(CmifCommandType_Control, token,
                MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen), NULL, 0);
        }
        return false;
    }

    switch (command_id) {
        case DGLAB_IPC_CMD_GET_VERSION: {
            const DglabIpcVersion version = {
                .major = 0,
                .minor = 1,
                .patch = 0,
            };
            dglabMakeResponse(CmifCommandType_Request, token, 0, &version, sizeof(version));
            break;
        }
        case DGLAB_IPC_CMD_PING: {
            const u32 pong = DGLAB_IPC_PING_MAGIC;
            dglabMakeResponse(CmifCommandType_Request, token, 0, &pong, sizeof(pong));
            break;
        }
        default:
            dglabMakeResponse(CmifCommandType_Request, token,
                MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen), NULL, 0);
            break;
    }

    return false;
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    Handle port = INVALID_HANDLE;
    Result rc = smRegisterService(&port, smEncodeName(DGLAB_IPC_SERVICE_NAME), false, 1);
    if (R_FAILED(rc))
        return rc;

    while (true) {
        Handle session = INVALID_HANDLE;
        rc = svcAcceptSession(&session, port);
        if (R_FAILED(rc)) {
            svcSleepThread(1000000ull);
            continue;
        }

        bool need_reply = false;
        bool close_after_reply = false;
        bool session_open = true;

        while (session_open) {
            s32 index = -1;
            Handle reply_target = need_reply ? session : (Handle)0;
            rc = svcReplyAndReceive(&index, &session, 1, reply_target, UINT64_MAX);

            if (R_FAILED(rc)) {
                session_open = false;
                break;
            }

            if (need_reply && close_after_reply) {
                session_open = false;
                break;
            }

            close_after_reply = dglabHandleRequest();
            need_reply = true;
        }

        svcCloseHandle(session);
    }

    smUnregisterService(smEncodeName(DGLAB_IPC_SERVICE_NAME));
    smExit();
    return 0;
}
