#include <stdio.h>
#include <string.h>

#include <switch.h>

#include <dglab/ipc.h>
#include <dglab/ipc_cmif.h>
#include <dglab/ipc_poc.h>
#include <dglab/transport/ble_poc.h>
#include <dglab/transport/net_socket.h>

_Static_assert(sizeof(DglabPocStatus) <= DGLAB_IPC_INLINE_PAYLOAD_MAX,
    "DglabPocStatus does not fit the inline IPC payload");
_Static_assert(sizeof(DglabPocLogChunk) <= DGLAB_IPC_INLINE_PAYLOAD_MAX,
    "DglabPocLogChunk does not fit the inline IPC payload");
_Static_assert(sizeof(DglabNetStatus) <= DGLAB_IPC_INLINE_PAYLOAD_MAX,
    "DglabNetStatus does not fit the inline IPC payload");
_Static_assert(sizeof(DglabNetQrChunk) <= DGLAB_IPC_INLINE_PAYLOAD_MAX,
    "DglabNetQrChunk does not fit the inline IPC payload");
_Static_assert(sizeof(DglabNetLogChunk) <= DGLAB_IPC_INLINE_PAYLOAD_MAX,
    "DglabNetLogChunk does not fit the inline IPC payload");

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

    // The word count has to include the alignment allowance described in
    // dglab/ipc_cmif.h. Without it the kernel copies only the CmifOutHeader and
    // truncates the payload, which makes clients read zeroed data.
    u32 num_data_words = dglabResponseDataWords(data_size);

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
                .minor = 2,
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
        // Wi-Fi + WebSocket transport (DG-LAB Socket V3).
        case DGLAB_IPC_CMD_NET_START: {
            DglabNetStartRequest request = { 0 };

            if (!dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(request))) {
                dglabMakeResponse(CmifCommandType_Request, token,
                    MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }

            memcpy(&request, dglabRequestPayload(in), sizeof(request));
            dglabMakeResponse(CmifCommandType_Request, token,
                dglabNetSocketStart((u16)request.port), NULL, 0);
            break;
        }
        case DGLAB_IPC_CMD_NET_STOP: {
            dglabMakeResponse(CmifCommandType_Request, token, dglabNetSocketStop(), NULL, 0);
            break;
        }
        case DGLAB_IPC_CMD_NET_STATUS: {
            DglabNetStatus status;
            Result rc = dglabNetSocketGetStatus(&status);

            dglabMakeResponse(CmifCommandType_Request, token, rc,
                R_SUCCEEDED(rc) ? &status : NULL, R_SUCCEEDED(rc) ? sizeof(status) : 0);
            break;
        }
        case DGLAB_IPC_CMD_NET_QR: {
            DglabNetQrChunk chunk;
            size_t size = 0;
            Result rc;

            memset(&chunk, 0, sizeof(chunk));
            rc = dglabNetSocketGetQr(chunk.text, sizeof(chunk.text), &size);

            if (R_SUCCEEDED(rc))
                chunk.size = (u32)size;

            dglabMakeResponse(CmifCommandType_Request, token, rc,
                R_SUCCEEDED(rc) ? &chunk : NULL, R_SUCCEEDED(rc) ? sizeof(chunk) : 0);
            break;
        }
        case DGLAB_IPC_CMD_NET_SEND: {
            DglabNetSendRequest request = { 0 };

            if (!dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(request))) {
                dglabMakeResponse(CmifCommandType_Request, token,
                    MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }

            memcpy(&request, dglabRequestPayload(in), sizeof(request));
            dglabMakeResponse(CmifCommandType_Request, token, dglabNetSocketSend(&request), NULL, 0);
            break;
        }
        case DGLAB_IPC_CMD_NET_LOG: {
            DglabNetLogRequest request = { 0 };
            DglabNetLogChunk chunk;

            if (dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(request)))
                memcpy(&request, dglabRequestPayload(in), sizeof(request));

            memset(&chunk, 0, sizeof(chunk));
            chunk.next_cursor = dglabNetSocketReadLog(request.cursor, chunk.text,
                sizeof(chunk.text));
            chunk.size = (u32)strlen(chunk.text);

            dglabMakeResponse(CmifCommandType_Request, token, 0, &chunk, sizeof(chunk));
            break;
        }

        // Temporary BLE transport PoC commands, see common/include/dglab/ipc_poc.h.
        case DGLAB_IPC_POC_CMD_START: {
            DglabPocStartRequest request = { 0 };

            if (!dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(request))) {
                dglabMakeResponse(CmifCommandType_Request, token,
                    MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }

            memcpy(&request, dglabRequestPayload(in), sizeof(request));
            dglabMakeResponse(CmifCommandType_Request, token, blePocStart(&request), NULL, 0);
            break;
        }
        case DGLAB_IPC_POC_CMD_STOP: {
            dglabMakeResponse(CmifCommandType_Request, token, blePocStop(), NULL, 0);
            break;
        }
        case DGLAB_IPC_POC_CMD_STATUS: {
            DglabPocStatus status;

            blePocGetStatus(&status);
            dglabMakeResponse(CmifCommandType_Request, token, 0, &status, sizeof(status));
            break;
        }
        case DGLAB_IPC_POC_CMD_LOG: {
            DglabPocLogRequest request = { 0 };
            DglabPocLogChunk chunk;

            if (dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(request)))
                memcpy(&request, dglabRequestPayload(in), sizeof(request));

            memset(&chunk, 0, sizeof(chunk));
            chunk.next_cursor = blePocReadLog(request.cursor, chunk.text, sizeof(chunk.text));
            chunk.size = (u32)strlen(chunk.text);

            dglabMakeResponse(CmifCommandType_Request, token, 0, &chunk, sizeof(chunk));
            break;
        }
        case DGLAB_IPC_POC_CMD_ACTION: {
            DglabPocActionRequest request = { 0 };

            if (!dglabRequestHasPayload(parsed.meta.num_data_words, sizeof(request))) {
                dglabMakeResponse(CmifCommandType_Request, token,
                    MAKERESULT(Module_Libnx, LibnxError_BadInput), NULL, 0);
                break;
            }

            memcpy(&request, dglabRequestPayload(in), sizeof(request));
            dglabMakeResponse(CmifCommandType_Request, token, blePocAction(&request), NULL, 0);
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

    blePocInitialize();

    // The socket server is started at boot: the NRO only reads its state and
    // displays the QR code. A failed start stays visible through NET_STATUS, and
    // NET_START retries it.
    dglabNetSocketInitialize();
    dglabNetSocketStart((u16)DGLAB_NET_DEFAULT_PORT);

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
