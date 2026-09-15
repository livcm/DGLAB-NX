#include <dglab/net/net_server.h>

#include <dglab/net/dglab_socket.h>
#include <dglab/protocol/coyote_v3_session.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Defined with the command and waveform helpers further down.
static DglabNetSendResult sendCommand(DglabNetServer* server, DglabNetClient* client,
    const char* command, bool log);
static bool channelList(uint32_t channel, DglabSocketChannel out[2], size_t* count);
static void waveformPump(DglabNetServer* server, uint64_t now_ms);
static void waveformClearQueue(DglabNetWaveformQueue* queue);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void dglabNetServerLog(DglabNetServer* server, const char* fmt, ...)
{
    char line[192];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);

    if (len < 0)
        return;

    if ((size_t)len > sizeof(line) - 2)
        len = (int)(sizeof(line) - 2);

    line[len] = '\0';

    if (server->config.log_sink)
        server->config.log_sink(server->config.context, line);

    line[len++] = '\n';

    for (int i = 0; i < len; i++) {
        server->log[server->log_write_offset % DGLAB_NET_LOG_CAPACITY] = line[i];
        server->log_write_offset++;
    }
}

uint32_t dglabNetServerReadLog(DglabNetServer* server, uint32_t cursor, char* out, size_t out_size)
{
    uint32_t write = server->log_write_offset;
    uint32_t earliest;
    uint32_t count;

    if (out_size == 0)
        return cursor;

    earliest = (write > DGLAB_NET_LOG_CAPACITY) ? write - DGLAB_NET_LOG_CAPACITY : 0;

    if (server->log_valid_from > earliest)
        earliest = server->log_valid_from;

    if (cursor > write)
        cursor = write;

    if (cursor < earliest)
        cursor = earliest;

    count = write - cursor;

    if (count > out_size - 1)
        count = out_size - 1;

    for (uint32_t i = 0; i < count; i++)
        out[i] = server->log[(cursor + i) % DGLAB_NET_LOG_CAPACITY];

    out[count] = '\0';

    return cursor + count;
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

size_t dglabNetFormatUuid(const uint8_t bytes[16], char* out, size_t out_size)
{
    static const char digits[] = "0123456789abcdef";
    size_t written = 0;

    // 32 hex digits, four '-', and the terminator.
    if (out_size < 37)
        return 0;

    for (size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[written++] = '-';

        out[written++] = digits[bytes[i] >> 4];
        out[written++] = digits[bytes[i] & 0x0Fu];
    }

    out[written] = '\0';

    return written;
}

bool dglabNetParseTargetId(const char* target, char* out, size_t out_size)
{
    const char* start;
    const char* end;
    size_t len;

    if (!target || !out || out_size == 0)
        return false;

    const char* query = strstr(target, "tid=");

    if (query) {
        // V4 form: "/?tid=<id>".
        start = query + 4;
        end = start;

        while (*end && *end != '&' && *end != '#')
            end++;
    } else {
        // V3 form: "/<id>".
        start = target;

        while (*start == '/')
            start++;

        end = start;

        while (*end && *end != '/' && *end != '?' && *end != '&' && *end != '#')
            end++;
    }

    len = (size_t)(end - start);

    if (len == 0 || len >= out_size)
        return false;

    memcpy(out, start, len);
    out[len] = '\0';

    return true;
}

static void generateId(DglabNetServer* server, char* out, size_t out_size)
{
    uint8_t bytes[16];
    bool random_ok = false;

    if (server->config.fill_random)
        random_ok = server->config.fill_random(server->config.context, bytes, sizeof(bytes));

    if (!random_ok) {
        // A stable id keeps the QR code usable, at the cost of being guessable.
        // The counter keeps ids unique so the controller and the App never end
        // up sharing one.
        dglabNetServerLog(server, "random source unavailable, using a fallback id");
        memset(bytes, 0, sizeof(bytes));
        bytes[12] = (uint8_t)(server->fallback_counter >> 24);
        bytes[13] = (uint8_t)(server->fallback_counter >> 16);
        bytes[14] = (uint8_t)(server->fallback_counter >> 8);
        bytes[15] = (uint8_t)server->fallback_counter;
        server->fallback_counter++;
    }

    bytes[6] = (uint8_t)((bytes[6] & 0x0Fu) | 0x40u); // version 4
    bytes[8] = (uint8_t)((bytes[8] & 0x3Fu) | 0x80u); // RFC 4122 variant

    if (dglabNetFormatUuid(bytes, out, out_size) == 0)
        out[0] = '\0';
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

static DglabNetClient* findFreeClient(DglabNetServer* server)
{
    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (!server->clients[i].active)
            return &server->clients[i];
    }

    return NULL;
}

static DglabNetClient* findClient(DglabNetServer* server, const WsConn* conn)
{
    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (server->clients[i].active && server->clients[i].conn == conn)
            return &server->clients[i];
    }

    return NULL;
}

static DglabNetClient* findBoundClient(DglabNetServer* server)
{
    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (server->clients[i].active && server->clients[i].bound)
            return &server->clients[i];
    }

    return NULL;
}

static bool sendText(DglabNetServer* server, WsConn* conn, const char* text, size_t size)
{
    if (!conn || !wsConnSendText(conn, text, size)) {
        dglabNetServerLog(server, "send failed (%u bytes)", (unsigned)size);
        return false;
    }

    server->status.messages_out++;

    return true;
}

static uint32_t parseCode(const char* code)
{
    uint32_t value = 0;

    for (const char* p = code; *p >= '0' && *p <= '9'; p++)
        value = value * 10u + (uint32_t)(*p - '0');

    return value;
}

static void sendError(DglabNetServer* server, WsConn* conn, const char* code)
{
    char frame[WS_MAX_MESSAGE];
    size_t len = dglabSocketBuildMessage(frame, sizeof(frame), "error", server->controller_id, "",
        code);

    if (len)
        sendText(server, conn, frame, len);

    server->status.last_error = parseCode(code);

    dglabNetServerLog(server, "error %s sent to a client", code);
}

static void sendHeartbeat(DglabNetServer* server, DglabNetClient* client, uint64_t now_ms)
{
    char frame[WS_MAX_MESSAGE];
    // The reference server keeps the link alive with a heartbeat; the exact
    // payload is unverified, see docs/dglab-socket.md.
    // Addressed the way the App accepts: clientId is the sender (us, the
    // controller), targetId is the recipient (the App).
    size_t len = dglabSocketBuildMessage(frame, sizeof(frame), "heartbeat",
        server->controller_id, client->id, "200");

    if (len && sendText(server, client->conn, frame, len))
        server->status.heartbeats_sent++;

    server->last_heartbeat_ms = now_ms;
}

static uint64_t nowMs(DglabNetServer* server)
{
    if (!server->config.now_ms)
        return 0;

    return server->config.now_ms(server->config.context);
}

void dglabNetServerInit(DglabNetServer* server, const DglabNetServerConfig* config)
{
    memset(server, 0, sizeof(*server));

    server->config = *config;
    server->status.state = DglabNetState_Idle;
    server->status.port = config->port;
    server->status.app_feedback = DGLAB_NET_FEEDBACK_NONE;

    generateId(server, server->controller_id, sizeof(server->controller_id));

    dglabNetServerLog(server, "socket server core ready, controller id %s",
        server->controller_id[0] ? server->controller_id : "(none)");
}

void dglabNetServerSetListening(DglabNetServer* server, uint16_t port)
{
    server->status.port = port;
    server->status.state = DglabNetState_Listening;
    server->status.paired = 0;

    dglabNetServerLog(server, "listening on port %u", (unsigned)port);
}

void dglabNetServerSetStopped(DglabNetServer* server)
{
    server->status.state = DglabNetState_Stopped;
    server->status.clients = 0;
    server->status.paired = 0;
    server->status.peer_id[0] = '\0';

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++)
        server->clients[i].active = false;

    for (size_t i = 0; i < 2; i++)
        waveformClearQueue(&server->waveform[i]);

    dglabNetServerLog(server, "stopped");
}

void dglabNetServerSetFailed(DglabNetServer* server, uint32_t result)
{
    server->status.state = DglabNetState_Failed;
    server->status.last_result = result;

    dglabNetServerLog(server, "failed, rc=0x%08X", (unsigned)result);
}

bool dglabNetServerAttach(DglabNetServer* server, WsConn* conn)
{
    DglabNetClient* client = findFreeClient(server);
    char requested[DGLAB_NET_ID_LEN];

    if (!conn)
        return false;

    server->status.sessions++;

    if (server->status.state != DglabNetState_Listening &&
        server->status.state != DglabNetState_Paired) {
        sendError(server, conn, "500");
        return false;
    }

    if (!client) {
        sendError(server, conn, "500");
        dglabNetServerLog(server, "rejected a client: no free connection slot");
        return false;
    }

    // The reference server rejects a target that does not carry the controller
    // id with 210. Real devices turned out not to be trusted on that point: the
    // connection is paired anyway and the mismatch is logged, because rejecting
    // leaves the App spinning in "connecting" with nothing to go on. Tighten
    // this once the log shows what the App actually sends.
    if (!dglabNetParseTargetId(conn->target, requested, sizeof(requested)))
        dglabNetServerLog(server, "client connected without a client id in the target");
    else if (strcmp(requested, server->controller_id) != 0)
        dglabNetServerLog(server, "client id %s does not match %s, pairing anyway", requested,
            server->controller_id);

    if (findBoundClient(server)) {
        // 400: the id is already bound by another client.
        sendError(server, conn, "400");
        return false;
    }

    memset(client, 0, sizeof(*client));
    client->conn = conn;
    client->active = true;
    client->bound = true;
    client->connected_at_ms = nowMs(server);
    client->last_message_ms = client->connected_at_ms;
    generateId(server, client->id, sizeof(client->id));

    server->status.clients++;
    server->status.paired = 1;
    server->status.state = DglabNetState_Paired;
    snprintf((char*)server->status.peer_id, sizeof(server->status.peer_id), "%s", client->id);

    dglabNetServerLog(server, "app %s bound (target '%s')", client->id, conn->target);

    // Bind reply to the App: clientId is the controller, targetId is the App's
    // own id, which is how the App learns its id (docs/dglab-socket.md).
    {
        char frame[WS_MAX_MESSAGE];
        size_t len = dglabSocketBuildMessage(frame, sizeof(frame), "bind",
            server->controller_id, client->id, "200");

        if (!len || !sendText(server, conn, frame, len)) {
            dglabNetServerDetach(server, conn);
            return false;
        }
    }

    // Send one heartbeat right away: the App treats a controller that never
    // reports in as offline.
    sendHeartbeat(server, client, nowMs(server));

    return true;
}

void dglabNetServerDetach(DglabNetServer* server, WsConn* conn)
{
    DglabNetClient* client = findClient(server, conn);

    if (!client)
        return;

    dglabNetServerLog(server, client->bound ? "app %s disconnected" : "client %s left",
        client->id);

    bool was_bound = client->bound;

    memset(client, 0, sizeof(*client));

    // Nothing is playing on the App any more, so the queues start empty.
    for (size_t i = 0; i < 2; i++)
        waveformClearQueue(&server->waveform[i]);

    if (server->status.clients)
        server->status.clients--;

    if (was_bound) {
        server->status.paired = 0;
        server->status.peer_id[0] = '\0';

        if (server->status.state == DglabNetState_Paired)
            server->status.state = DglabNetState_Listening;
    }
}

// ---------------------------------------------------------------------------
// Messages from the App
// ---------------------------------------------------------------------------

static void handleAppMessage(DglabNetServer* server, const DglabSocketMessage* message)
{
    DglabSocketStrengthData strength;
    int button;

    if (dglabSocketParseStrengthReport(message->message, &strength)) {
        server->status.app_strength_a = (uint32_t)strength.a;
        server->status.app_strength_b = (uint32_t)strength.b;
        server->status.app_limit_a = (uint32_t)strength.a_limit;
        server->status.app_limit_b = (uint32_t)strength.b_limit;
        server->status.reports_received++;
        return;
    }

    if (dglabSocketParseFeedback(message->message, &button)) {
        server->status.app_feedback = (uint32_t)button;
        server->status.reports_received++;
        return;
    }

    // Anything else is reported by a newer App than this build knows about.
    // Logging it keeps the traffic visible without failing the connection.
    dglabNetServerLog(server, "unhandled app message: %s", message->message);
}

// The App has been seen to stay completely silent after a successful bind, so
// every incoming type is logged, not only the ones this build understands.
#define DGLAB_NET_LOG_ALL_TYPES 32u

void dglabNetServerOnMessage(DglabNetServer* server, WsConn* conn, const char* text, size_t size)
{
    DglabNetClient* client = findClient(server, conn);
    DglabSocketMessage message;

    if (!client)
        return;

    client->messages_in++;
    client->last_message_ms = nowMs(server);
    client->warned_silent = false;
    server->status.messages_in++;

    if (size > DGLAB_SOCKET_MAX_MESSAGE) {
        sendError(server, conn, "405");
        return;
    }

    if (!dglabSocketParseMessage(text, size, &message)) {
        sendError(server, conn, "403");
        return;
    }

    switch (message.type) {
        case DglabSocketType_Msg:
            if (server->messages_logged < DGLAB_NET_LOG_MESSAGE_LIMIT) {
                server->messages_logged++;
                dglabNetServerLog(server, "rx msg: %s", message.message);
            }
            handleAppMessage(server, &message);
            break;

        case DglabSocketType_Heartbeat:
            // Logged a few times: whether the App sends heartbeats at all is
            // exactly the open question when commands are ignored.
            if (server->messages_logged < DGLAB_NET_LOG_ALL_TYPES) {
                server->messages_logged++;
                dglabNetServerLog(server, "rx heartbeat");
            }
            break;

        case DglabSocketType_Break:
            client->bound = false;
            server->status.paired = 0;
            server->status.peer_id[0] = '\0';

            if (server->status.state == DglabNetState_Paired)
                server->status.state = DglabNetState_Listening;

            dglabNetServerLog(server, "app %s reported a broken binding", client->id);
            break;

        default:
            dglabNetServerLog(server, "ignored type '%s' from the app",
                dglabSocketTypeName(message.type));
            break;
    }
}

void dglabNetServerOnActivity(DglabNetServer* server, WsConn* conn)
{
    DglabNetClient* client = findClient(server, conn);

    if (!client)
        return;

    client->last_message_ms = nowMs(server);
    client->warned_silent = false;
}

void dglabNetServerPoll(DglabNetServer* server, uint64_t now_ms)
{
    DglabNetClient* client = findBoundClient(server);

    if (!client)
        return;

    waveformPump(server, now_ms);

    if (now_ms - server->last_heartbeat_ms >= DGLAB_NET_HEARTBEAT_INTERVAL_MS)
        sendHeartbeat(server, client, now_ms);

    if (!client->warned_silent &&
        now_ms - client->last_message_ms >= DGLAB_NET_HEARTBEAT_INTERVAL_MS * 3u) {
        client->warned_silent = true;
        dglabNetServerLog(server, "app %s has been quiet for %u s", client->id,
            (unsigned)(DGLAB_NET_HEARTBEAT_INTERVAL_MS * 3u / 1000u));
    }
}

// ---------------------------------------------------------------------------
// Commands to the App
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Waveform streaming
//
// The App plays a pulse segment once and stops, so a continuous waveform only
// keeps going if it is fed. Slots come from an event source (a game hit, a
// motion sensor), are queued per channel, and leave as pulse commands slightly
// ahead of playback. The channel strength is untouched: the user owns it, and
// the device multiplies it with the waveform strength.
// ---------------------------------------------------------------------------

static DglabNetWaveformQueue* waveformQueue(DglabNetServer* server, DglabSocketChannel channel)
{
    return &server->waveform[(channel == DglabSocketChannel_A) ? 0 : 1];
}

static void waveformClearQueue(DglabNetWaveformQueue* queue)
{
    queue->head = 0;
    queue->count = 0;
    queue->next_send_ms = 0;
}

static void waveformDrop(DglabNetWaveformQueue* queue, size_t count)
{
    if (count > queue->count)
        count = queue->count;

    queue->head = (queue->head + count) % DGLAB_NET_WAVEFORM_QUEUE_SLOTS;
    queue->count -= count;
}

static void waveformPush(DglabNetServer* server, DglabSocketChannel channel,
    const DglabNetWaveformSlot* slots, size_t count)
{
    DglabNetWaveformQueue* queue = waveformQueue(server, channel);

    if (count > DGLAB_NET_WAVEFORM_QUEUE_SLOTS)
        count = DGLAB_NET_WAVEFORM_QUEUE_SLOTS;

    // A source that falls this far behind is better served by the newest
    // material, so the oldest slots make room for it.
    if (queue->count + count > DGLAB_NET_WAVEFORM_QUEUE_SLOTS) {
        size_t drop = queue->count + count - DGLAB_NET_WAVEFORM_QUEUE_SLOTS;

        waveformDrop(queue, drop);
        dglabNetServerLog(server, "waveform queue was full, dropped %u slots", (unsigned)drop);
    }

    for (size_t i = 0; i < count; i++) {
        size_t index = (queue->head + queue->count) % DGLAB_NET_WAVEFORM_QUEUE_SLOTS;

        queue->slots[index] = slots[i];
        queue->count++;
    }
}

// Sends one pulse command with as much queued material as fits. Returns false
// when there was nothing whole to send (fewer than four slots, which is one
// element).
static bool waveformSendBatch(DglabNetServer* server, DglabNetClient* client,
    DglabSocketChannel channel, uint64_t now_ms)
{
    DglabNetWaveformQueue* queue = waveformQueue(server, channel);
    char storage[DGLAB_NET_WAVEFORM_BATCH_SLOTS / DGLAB_COYOTE_V3_WAVEFORM_SLOTS][17];
    const char* hex[DGLAB_NET_WAVEFORM_BATCH_SLOTS / DGLAB_COYOTE_V3_WAVEFORM_SLOTS];
    DglabCoyoteV3WaveformSlot group[DGLAB_COYOTE_V3_WAVEFORM_SLOTS];
    char command[DGLAB_SOCKET_MAX_MESSAGE];
    size_t elements = queue->count / DGLAB_COYOTE_V3_WAVEFORM_SLOTS;
    size_t slots;
    size_t len;
    DglabNetSendResult result;

    if (elements == 0)
        return false;

    if (elements > DGLAB_NET_WAVEFORM_BATCH_SLOTS / DGLAB_COYOTE_V3_WAVEFORM_SLOTS)
        elements = DGLAB_NET_WAVEFORM_BATCH_SLOTS / DGLAB_COYOTE_V3_WAVEFORM_SLOTS;

    for (size_t element = 0; element < elements; element++) {
        for (size_t slot = 0; slot < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; slot++) {
            const DglabNetWaveformSlot* source =
                &queue->slots[(queue->head + element * DGLAB_COYOTE_V3_WAVEFORM_SLOTS + slot) %
                              DGLAB_NET_WAVEFORM_QUEUE_SLOTS];

            // The protocol layer owns the app side frequency to device value
            // conversion, so it is reused here rather than duplicated.
            group[slot].frequency = dglabCoyoteV3CompressFrequency(source->frequency_ms);
            group[slot].strength = source->strength;
        }

        dglabSocketEncodePulseHex(group, storage[element]);
        hex[element] = storage[element];
    }

    len = dglabSocketBuildPulse(command, sizeof(command), channel, hex, elements);

    if (len == 0)
        return false;

    slots = elements * DGLAB_COYOTE_V3_WAVEFORM_SLOTS;
    result = sendCommand(server, client, command, false);

    if (result != DglabNetSend_Ok)
        return false;

    waveformDrop(queue, slots);
    server->status.commands_sent++;

    // Next batch when the App is down to the lead time, so playback never runs
    // dry but the queue stays short enough to react to a new event.
    {
        uint64_t lead = (uint64_t)slots * DGLAB_NET_WAVEFORM_SLOT_MS;

        queue->next_send_ms = now_ms + ((lead > DGLAB_NET_WAVEFORM_LEAD_MS)
                                           ? (lead - DGLAB_NET_WAVEFORM_LEAD_MS)
                                           : 0);
    }

    return true;
}

// Feeds every channel that is due. Called from the periodic poll.
static void waveformPump(DglabNetServer* server, uint64_t now_ms)
{
    DglabNetClient* client = findBoundClient(server);

    if (!client)
        return;

    for (size_t i = 0; i < 2; i++) {
        DglabSocketChannel channel = (i == 0) ? DglabSocketChannel_A : DglabSocketChannel_B;
        DglabNetWaveformQueue* queue = &server->waveform[i];

        if (queue->count < DGLAB_COYOTE_V3_WAVEFORM_SLOTS) {
            // Nothing whole is left: the App should send again as soon as the
            // next slots arrive.
            queue->next_send_ms = now_ms;
            continue;
        }

        if (now_ms >= queue->next_send_ms)
            waveformSendBatch(server, client, channel, now_ms);
    }
}

DglabNetSendResult dglabNetServerUploadWaveform(DglabNetServer* server,
    const DglabNetWaveformRequest* request)
{
    DglabNetClient* client = findBoundClient(server);
    DglabSocketChannel channels[2];
    size_t channel_count = 0;

    if (request->mode != DglabNetWaveform_Append && request->mode != DglabNetWaveform_Replace)
        return DglabNetSend_BadRequest;

    if (request->slot_count == 0 || request->slot_count > DGLAB_NET_WAVEFORM_MAX_SLOTS)
        return DglabNetSend_BadRequest;

    if (!channelList(request->channel, channels, &channel_count))
        return DglabNetSend_BadRequest;

    for (size_t i = 0; i < request->slot_count; i++) {
        const DglabNetWaveformSlot* slot = &request->slots[i];

        if (slot->frequency_ms < DGLAB_COYOTE_V3_WAVEFORM_FREQUENCY_MIN_MS ||
            slot->frequency_ms > DGLAB_COYOTE_V3_WAVEFORM_FREQUENCY_MAX_MS ||
            slot->strength > DGLAB_COYOTE_V3_WAVEFORM_STRENGTH_MAX)
            return DglabNetSend_BadRequest;
    }

    if (!client)
        return DglabNetSend_NotPaired;

    for (size_t i = 0; i < channel_count; i++) {
        DglabNetWaveformQueue* queue = waveformQueue(server, channels[i]);
        DglabNetSendResult result;

        if (request->mode == DglabNetWaveform_Replace) {
            // An event replaces the gesture that is playing, so both our queue
            // and the App's have to be dropped before the new slots go out.
            char command[DGLAB_SOCKET_MAX_MESSAGE];

            waveformClearQueue(queue);

            if (dglabSocketBuildClear(command, sizeof(command), channels[i]) != 0) {
                result = sendCommand(server, client, command, true);

                if (result != DglabNetSend_Ok)
                    return result;
            }
        }

        waveformPush(server, channels[i], request->slots, request->slot_count);

        if (request->mode == DglabNetWaveform_Replace)
            waveformSendBatch(server, client, channels[i], nowMs(server));
        else
            waveformPump(server, nowMs(server));
    }

    return DglabNetSend_Ok;
}

static bool channelList(uint32_t channel, DglabSocketChannel out[2], size_t* count)
{
    switch (channel) {
        case 0:
            out[0] = DglabSocketChannel_A;
            out[1] = DglabSocketChannel_B;
            *count = 2;
            return true;
        case 1:
            out[0] = DglabSocketChannel_A;
            *count = 1;
            return true;
        case 2:
            out[0] = DglabSocketChannel_B;
            *count = 1;
            return true;
        default:
            return false;
    }
}

static DglabNetSendResult sendCommand(DglabNetServer* server, DglabNetClient* client,
    const char* command, bool log)
{
    // The command and the frame are big (the pulse command with its eight
    // elements is the largest message this build ever sends) and the sysmodule's
    // main thread only has a 16KB stack, so both buffers live in .bss. All sends
    // happen under the transport lock, so one set of buffers is enough.
    static char frame[WS_MAX_MESSAGE];
    // clientId is the sender and targetId the recipient, which is what the App
    // acts on: with the two swapped it logged the message and ignored it, which
    // is how this was found (see docs/dglab-socket.md).
    size_t len = dglabSocketBuildMessage(frame, sizeof(frame), "msg", server->controller_id,
        client->id, command);

    if (len == 0 || len > DGLAB_SOCKET_MAX_MESSAGE)
        return DglabNetSend_TooLong;

    // Logged before the socket call, so a crash inside the write still leaves a
    // trace of how far this message got. Waveform batches are silenced: they are
    // long and arrive continuously, and would push everything else out of the
    // ring.
    if (log)
        dglabNetServerLog(server, "tx %s", command);

    if (!sendText(server, client->conn, frame, len))
        return DglabNetSend_IoError;

    return DglabNetSend_Ok;
}

// A short, mild waveform for the "test" button: eight 100ms elements at one
// fixed frequency, which is 0.8s of output. Wallclock length matters more than
// the shape here, the point is to prove the whole path works.
#define DGLAB_NET_TEST_PULSE_ELEMENTS 8u
#define DGLAB_NET_TEST_PULSE_FREQUENCY_MS 100u
#define DGLAB_NET_TEST_PULSE_DEFAULT_STRENGTH 10u

static bool buildTestPulse(char* out, size_t out_size, DglabSocketChannel channel,
    uint32_t strength)
{
    DglabCoyoteV3WaveformSlot slots[DGLAB_COYOTE_V3_WAVEFORM_SLOTS];
    char storage[DGLAB_NET_TEST_PULSE_ELEMENTS][17];
    const char* hex[DGLAB_NET_TEST_PULSE_ELEMENTS];
    uint8_t level = (uint8_t)(strength ? strength : DGLAB_NET_TEST_PULSE_DEFAULT_STRENGTH);
    uint8_t frequency = dglabCoyoteV3CompressFrequency(DGLAB_NET_TEST_PULSE_FREQUENCY_MS);

    for (size_t i = 0; i < DGLAB_COYOTE_V3_WAVEFORM_SLOTS; i++) {
        slots[i].frequency = frequency;
        slots[i].strength = level;
    }

    for (size_t i = 0; i < DGLAB_NET_TEST_PULSE_ELEMENTS; i++) {
        dglabSocketEncodePulseHex(slots, storage[i]);
        hex[i] = storage[i];
    }

    return dglabSocketBuildPulse(out, out_size, channel, hex, DGLAB_NET_TEST_PULSE_ELEMENTS) != 0;
}

DglabNetSendResult dglabNetServerSend(DglabNetServer* server, const DglabNetSendRequest* request)
{
    DglabNetClient* client = findBoundClient(server);
    DglabSocketChannel channels[2];
    size_t channel_count = 0;
    static char command[DGLAB_SOCKET_MAX_MESSAGE];
    DglabNetSendResult result = DglabNetSend_Ok;

    if (!client)
        return DglabNetSend_NotPaired;

    if (!channelList(request->channel, channels, &channel_count))
        return DglabNetSend_BadRequest;

    switch (request->command) {
        case DglabNetCommand_SetStrength:
            if (request->value > DGLAB_COYOTE_V3_STRENGTH_MAX)
                return DglabNetSend_BadRequest;

            for (size_t i = 0; i < channel_count && result == DglabNetSend_Ok; i++) {
                size_t len = dglabSocketBuildStrength(command, sizeof(command), channels[i],
                    DglabSocketStrength_SetTo, (int)request->value);

                if (len == 0) {
                    result = DglabNetSend_TooLong;
                    break;
                }

                result = sendCommand(server, client, command, true);
            }
            break;

        case DglabNetCommand_Clear:
            for (size_t i = 0; i < channel_count && result == DglabNetSend_Ok; i++) {
                size_t len = dglabSocketBuildClear(command, sizeof(command), channels[i]);

                if (len == 0) {
                    result = DglabNetSend_TooLong;
                    break;
                }

                // Clearing the channel also stops the waveform stream, which is
                // what a user pressing "stop" expects.
                waveformClearQueue(waveformQueue(server, channels[i]));

                result = sendCommand(server, client, command, true);
            }
            break;

        case DglabNetCommand_IncreaseStrength:
        case DglabNetCommand_DecreaseStrength:
            // The protocol calls these relative changes: the App adds or
            // subtracts the value from whatever the channel is set to, so an
            // event source can react without knowing the current level.
            if (request->value > DGLAB_COYOTE_V3_STRENGTH_MAX)
                return DglabNetSend_BadRequest;

            for (size_t i = 0; i < channel_count && result == DglabNetSend_Ok; i++) {
                DglabSocketStrengthOp op = (request->command == DglabNetCommand_IncreaseStrength)
                                               ? DglabSocketStrength_Increase
                                               : DglabSocketStrength_Decrease;
                size_t len = dglabSocketBuildStrength(command, sizeof(command), channels[i], op,
                    (int)request->value);

                if (len == 0) {
                    result = DglabNetSend_TooLong;
                    break;
                }

                result = sendCommand(server, client, command, true);
            }
            break;

        case DglabNetCommand_TestPulse:
            if (request->value > DGLAB_COYOTE_V3_WAVEFORM_STRENGTH_MAX)
                return DglabNetSend_BadRequest;

            for (size_t i = 0; i < channel_count && result == DglabNetSend_Ok; i++) {
                if (!buildTestPulse(command, sizeof(command), channels[i], request->value)) {
                    result = DglabNetSend_TooLong;
                    break;
                }

                result = sendCommand(server, client, command, true);
            }
            break;

        default:
            return DglabNetSend_BadRequest;
    }

    if (result == DglabNetSend_Ok) {
        server->status.commands_sent++;
        dglabNetServerLog(server, "tx command %u channel %u value %u",
            (unsigned)request->command, (unsigned)request->channel, (unsigned)request->value);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Status, QR and network address
// ---------------------------------------------------------------------------

static void refreshAddress(DglabNetServer* server)
{
    uint32_t address = 0;
    char text[sizeof(server->status.ip_text)];

    text[0] = '\0';

    if (server->config.get_ip &&
        server->config.get_ip(server->config.context, &address, text, sizeof(text))) {
        server->status.ip = address;
        snprintf((char*)server->status.ip_text, sizeof(server->status.ip_text), "%s", text);
        return;
    }

    server->status.ip = 0;
    server->status.ip_text[0] = '\0';
}

void dglabNetServerGetStatus(DglabNetServer* server, DglabNetStatus* out)
{
    refreshAddress(server);
    *out = server->status;
}

bool dglabNetServerGetQr(DglabNetServer* server, char* out, size_t out_size, size_t* out_written)
{
    char uri[64];
    size_t uri_len;
    size_t len;

    if (out_written)
        *out_written = 0;

    if (!out || out_size == 0)
        return false;

    refreshAddress(server);

    if (server->status.ip_text[0] == '\0') {
        out[0] = '\0';
        return false;
    }

    uri_len = (size_t)snprintf(uri, sizeof(uri), "ws://%s:%u", (const char*)server->status.ip_text,
        (unsigned)server->status.port);

    if (uri_len == 0 || uri_len >= sizeof(uri)) {
        out[0] = '\0';
        return false;
    }

    len = dglabSocketBuildQrUrl(out, out_size, uri, server->controller_id);

    if (len == 0) {
        out[0] = '\0';
        return false;
    }

    if (out_written)
        *out_written = len;

    return true;
}
