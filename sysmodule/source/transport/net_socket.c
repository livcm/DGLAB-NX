#include <dglab/transport/net_socket.h>

#include <dglab/net/net_server.h>

#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// Handshake reads are bounded so a stalled peer cannot hold a slot forever;
// once the connection is up the reads block, and a shutdown() is what stops the
// thread. There is no idle timeout yet, see docs/dglab-socket.md.
#define NET_HANDSHAKE_TIMEOUT_S 10
#define NET_TRANSFER_TIMEOUT_S 5

#define NET_LISTEN_BACKLOG 2
#define NET_THREAD_STACK_SIZE 0x4000
#define NET_THREAD_PRIORITY 0x2C
#define NET_TICK_INTERVAL_MS 100
#define NET_TICK_INTERVAL_NS (NET_TICK_INTERVAL_MS * 1000000ull)

typedef struct {
    int fd; ///< -1 while the slot is unused
    Thread thread;
    bool thread_valid; ///< a handle exists and has to be joined before reuse
    bool active;       ///< a running connection owns the slot
    Mutex write_mutex; ///< serialises frames from the core and from wsConnRecv
    bool write_mutex_ready;
    WsConn conn;
} NetSocketClient;

static struct {
    Mutex mutex;
    bool mutex_ready;
    DglabNetServer server;
    bool core_ready;
    bool bsd_ready;
    bool nifm_ready;
    bool csprng_ready;
    bool running;
    u16 requested_port; // port to bring back up after a sleep
    bool restart_after_sleep;
    uint64_t last_activity_ms;
    volatile bool stopping;
    int listen_fd;
    Thread accept_thread;
    bool accept_valid;
    Thread tick_thread;
    bool tick_valid;
    NetSocketClient clients[DGLAB_NET_MAX_CLIENTS];
} g_net = {
    .listen_fd = -1,
    .clients = {
        { .fd = -1, .active = false, .thread_valid = false },
        { .fd = -1, .active = false, .thread_valid = false },
    },
};

// ---------------------------------------------------------------------------
// Platform callbacks for the platform independent core
// ---------------------------------------------------------------------------

static uint64_t netNowMs(void* context)
{
    (void)context;

    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

static bool netFillRandom(void* context, uint8_t* out, size_t size)
{
    (void)context;

    if (!g_net.csprng_ready)
        return false;

    return R_SUCCEEDED(csrngGetRandomBytes(out, size));
}

static bool netGetIp(void* context, uint32_t* address, char* text, size_t text_size)
{
    u32 ip = 0;
    Result rc;

    (void)context;

    if (!g_net.nifm_ready)
        return false;

    rc = nifmGetCurrentIpAddress(&ip);

    if (R_FAILED(rc) || ip == 0)
        return false;

    // nifm reports the address as struct in_addr, so the bytes are already in
    // the order a dotted quad needs.
    const u8* bytes = (const u8*)&ip;

    snprintf(text, text_size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    *address = ip;

    return true;
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

static void netLog(const char* fmt, ...)
{
    char line[160];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    mutexLock(&g_net.mutex);
    dglabNetServerLog(&g_net.server, "%s", line);
    mutexUnlock(&g_net.mutex);
}

// Creates the server core on first use (defined below, next to the public API).
static void netCoreEnsureReady(u16 port);

static void netSetTimeout(int fd, int option, int seconds)
{
    struct timeval timeout;

    timeout.tv_sec = (time_t)seconds;
    timeout.tv_usec = 0;

    setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

// Dotted quad of the peer, for the log. Logs show which address actually
// reached the console, which is the first thing to check when the App hangs.
static void netPeerText(int fd, char* out, size_t out_size)
{
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);

    snprintf(out, out_size, "?");

    if (getpeername(fd, (struct sockaddr*)&peer, &length) != 0)
        return;

    if (inet_ntop(AF_INET, &peer.sin_addr, out, (socklen_t)out_size) == NULL)
        snprintf(out, out_size, "?");
}

static int netSocketRead(void* context, uint8_t* buffer, size_t size)
{
    NetSocketClient* slot = context;
    int fd = slot->fd;
    ssize_t rc;

    do {
        rc = recv(fd, buffer, size, 0);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0)
        return (rc == 0) ? 0 : -1;

    return (int)rc;
}

static int netSocketWrite(void* context, const uint8_t* buffer, size_t size)
{
    NetSocketClient* slot = context;
    int fd = slot->fd;
    size_t written = 0;
    int result;

    // The core (heartbeats, commands) and the client thread (pong and close
    // replies inside wsConnRecv) both write to the same socket, so frames have
    // to be serialised here or they interleave.
    mutexLock(&slot->write_mutex);

    while (written < size) {
        ssize_t rc = send(fd, buffer + written, size - written, 0);

        if (rc < 0 && errno == EINTR)
            continue;

        if (rc <= 0) {
            mutexUnlock(&slot->write_mutex);
            return -1;
        }

        written += (size_t)rc;
    }

    result = (int)size;

    mutexUnlock(&slot->write_mutex);

    return result;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

static void netClientThreadMain(void* arg)
{
    NetSocketClient* slot = &g_net.clients[(size_t)(uintptr_t)arg];
    WsConn* conn = &slot->conn;
    bool attached = false;
    char peer[32];

    netSetTimeout(slot->fd, SO_RCVTIMEO, NET_HANDSHAKE_TIMEOUT_S);
    netSetTimeout(slot->fd, SO_SNDTIMEO, NET_TRANSFER_TIMEOUT_S);

    netPeerText(slot->fd, peer, sizeof(peer));

    if (wsConnHandshake(conn)) {
        mutexLock(&g_net.mutex);
        dglabNetServerLog(&g_net.server, "websocket from %s, target '%s'", peer, conn->target);
        attached = dglabNetServerAttach(&g_net.server, conn);
        mutexUnlock(&g_net.mutex);
    } else {
        netLog("websocket handshake from %s failed", peer);
    }

    if (attached) {
        netSetTimeout(slot->fd, SO_RCVTIMEO, 0);

        for (;;) {
            uint8_t payload[WS_MAX_MESSAGE];
            size_t size = 0;
            WsOpcode opcode;

            if (!wsConnRecv(conn, &opcode, payload, sizeof(payload), &size))
                break;

            if (opcode != WsOpcode_Text && opcode != WsOpcode_Binary)
                continue;

            mutexLock(&g_net.mutex);
            dglabNetServerOnMessage(&g_net.server, conn, (const char*)payload, size);
            mutexUnlock(&g_net.mutex);
        }
    }

    mutexLock(&g_net.mutex);
    dglabNetServerDetach(&g_net.server, conn);
    mutexUnlock(&g_net.mutex);

    shutdown(slot->fd, SHUT_RDWR);
    close(slot->fd);

    // Release the slot for the next connection. The thread handle stays valid
    // until the accept thread joins it.
    mutexLock(&g_net.mutex);
    slot->fd = -1;
    slot->active = false;
    mutexUnlock(&g_net.mutex);
}

static void netStartClient(int fd)
{
    NetSocketClient* slot = NULL;
    size_t index = 0;
    Thread previous;
    bool join_previous = false;
    Result rc;

    netSetTimeout(fd, SO_SNDTIMEO, NET_TRANSFER_TIMEOUT_S);

    // Logged before the handshake: without this, "the phone never reached us"
    // and "the phone connected but the handshake never finished" look the same
    // in the log.
    {
        char peer[32];

        netPeerText(fd, peer, sizeof(peer));
        netLog("accept from %s", peer);

        mutexLock(&g_net.mutex);
        g_net.last_activity_ms = netNowMs(NULL);
        mutexUnlock(&g_net.mutex);
    }

    mutexLock(&g_net.mutex);

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (!g_net.clients[i].active) {
            slot = &g_net.clients[i];
            index = i;
            break;
        }
    }

    if (!slot) {
        mutexUnlock(&g_net.mutex);
        netLog("rejected a connection: every slot is busy");
        close(fd);
        return;
    }

    if (slot->thread_valid) {
        previous = slot->thread;
        slot->thread_valid = false;
        join_previous = true;
    }

    mutexUnlock(&g_net.mutex);

    // Join before touching the slot: the previous thread clears the slot as its
    // last step, and it must not clear the fd this connection is about to use.
    if (join_previous) {
        threadWaitForExit(&previous);
        threadClose(&previous);
    }

    mutexLock(&g_net.mutex);

    if (g_net.stopping) {
        mutexUnlock(&g_net.mutex);
        close(fd);
        return;
    }

    memset(&slot->conn, 0, sizeof(slot->conn));
    slot->conn.read = netSocketRead;
    slot->conn.write = netSocketWrite;
    slot->conn.context = slot;
    slot->fd = fd;
    slot->active = true;

    if (!slot->write_mutex_ready) {
        mutexInit(&slot->write_mutex);
        slot->write_mutex_ready = true;
    }

    rc = threadCreate(&slot->thread, netClientThreadMain, (void*)(uintptr_t)index, NULL,
        NET_THREAD_STACK_SIZE, NET_THREAD_PRIORITY, -2);

    if (R_SUCCEEDED(rc))
        rc = threadStart(&slot->thread);

    if (R_FAILED(rc)) {
        dglabNetServerLog(&g_net.server, "client thread failed rc=0x%08X", (unsigned)rc);
        slot->fd = -1;
        slot->active = false;
        // A Thread that was created but never started is left for the accept
        // thread to overwrite: it cannot be waited for, and a failed start is a
        // system level failure anyway.
        mutexUnlock(&g_net.mutex);
        close(fd);
        return;
    }

    slot->thread_valid = true;

    mutexUnlock(&g_net.mutex);
}

static void netAcceptThreadMain(void* arg)
{
    (void)arg;

    while (!g_net.stopping) {
        struct pollfd pfd;
        int fd;

        pfd.fd = g_net.listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, NET_TICK_INTERVAL_MS) <= 0)
            continue;

        fd = accept(g_net.listen_fd, NULL, NULL);

        if (fd < 0)
            continue;

        netStartClient(fd);
    }
}

static void netTickThreadMain(void* arg)
{
    (void)arg;

    while (!g_net.stopping) {
        svcSleepThread((u64)NET_TICK_INTERVAL_NS);

        mutexLock(&g_net.mutex);
        dglabNetServerPoll(&g_net.server, netNowMs(NULL));
        mutexUnlock(&g_net.mutex);
    }
}

// ---------------------------------------------------------------------------
// Sleep handling
//
// Holding sockets across a system sleep is what makes a console hang, so the
// server is torn down when the power state coordinator asks the console to
// sleep and started again when it wakes up. There is no precedence for this in
// the examples, so a failed registration (the module id may be taken by a system
// process) is logged and the server simply keeps running.
// ---------------------------------------------------------------------------

static PscPmModule g_pm_module;
static bool g_pm_registered;
static Thread g_pm_thread;

static void netSleepStopServer(void)
{
    bool was_running;

    mutexLock(&g_net.mutex);
    was_running = g_net.running;
    mutexUnlock(&g_net.mutex);

    if (!was_running)
        return;

    netLog("sleep requested, stopping the socket server");
    dglabNetSocketStop();

    mutexLock(&g_net.mutex);
    g_net.restart_after_sleep = true;
    mutexUnlock(&g_net.mutex);
}

static void netSleepResumeServer(void)
{
    bool restart;
    u16 port;

    mutexLock(&g_net.mutex);
    restart = g_net.restart_after_sleep;
    port = g_net.requested_port;
    g_net.restart_after_sleep = false;
    mutexUnlock(&g_net.mutex);

    if (!restart)
        return;

    netLog("woke up, starting the socket server again");
    dglabNetSocketStart(port);
}

// Seconds with nobody connected before the server closes itself. Holding the
// listening socket across a sleep is what hangs the console, and there is no
// reliable sleep notification, so a forgotten server is put away on its own.
#define NET_IDLE_STOP_MS (5u * 60u * 1000u)

static void netCheckIdleStop(void)
{
    bool stop = false;
    uint64_t now = netNowMs(NULL);

    mutexLock(&g_net.mutex);

    if (g_net.running && g_net.server.status.clients == 0 &&
        g_net.last_activity_ms != 0 &&
        now - g_net.last_activity_ms >= NET_IDLE_STOP_MS)
        stop = true;

    mutexUnlock(&g_net.mutex);

    if (stop) {
        netLog("no client for %u minutes, stopping the server before it can be slept",
            (unsigned)(NET_IDLE_STOP_MS / 60000u));
        dglabNetSocketStop();
    }
}

static void netSleepThreadMain(void* arg)
{
    (void)arg;

    for (;;) {
        PscPmState state;
        u32 flags;

        if (g_pm_registered) {
            // The event is not autoclear, so a timeout just means "nothing yet".
            eventWait(&g_pm_module.event, 1000000000ull);
        } else {
            svcSleepThread(1000000000ull);
        }

        netCheckIdleStop();

        while (g_pm_registered &&
               R_SUCCEEDED(pscPmModuleGetRequest(&g_pm_module, &state, &flags))) {
            switch (state) {
                case PscPmState_ReadySleep:
                case PscPmState_ReadyShutdown:
                    netSleepStopServer();
                    break;

                case PscPmState_ReadyAwaken:
                    netSleepResumeServer();
                    break;

                default:
                    break;
            }

            // Every request has to be answered, or the system waits for us.
            pscPmModuleAcknowledge(&g_pm_module, state);
        }
    }
}

void dglabNetSocketStartSleepWatch(void)
{
    static const u32 dependencies[] = { PscPmModuleId_WlanSockets };
    // The system owns the WlanSockets id (hardware answered 0x0000108A for it),
    // so a free id is tried afterwards. Registering with an id nothing else uses
    // only means the system waits for our acknowledgement, which it gets; this
    // is an experiment and the log names the id that worked.
    static const PscPmModuleId kCandidateIds[] = {
        PscPmModuleId_WlanSockets,
        200,
        201,
    };
    Result rc;
    Result last_rc = 0;
    size_t candidate;

    dglabNetSocketInitialize();

    // The core has to exist before the power state is watched, so that its log
    // (and the SD card copy of it) works from the first line on.
    mutexLock(&g_net.mutex);
    netCoreEnsureReady((u16)DGLAB_NET_DEFAULT_PORT);
    mutexUnlock(&g_net.mutex);

    rc = pscmInitialize();

    if (R_SUCCEEDED(rc)) {
        for (candidate = 0; candidate < sizeof(kCandidateIds) / sizeof(kCandidateIds[0]);
             candidate++) {
            rc = pscmGetPmModule(&g_pm_module, kCandidateIds[candidate], dependencies, 1, false);

            if (R_SUCCEEDED(rc)) {
                netLog("sleep watch registered as module %u",
                    (unsigned)kCandidateIds[candidate]);
                break;
            }

            last_rc = rc;
        }
    } else {
        last_rc = rc;
    }

    if (R_SUCCEEDED(rc)) {
        g_pm_registered = true;
    } else {
        // The server still runs; it just cannot be told to step aside for sleep,
        // which is why it stops itself when idle and why the NRO asks the user
        // to stop it before sleeping.
        netLog("sleep watch unavailable rc=0x%08X, the server cannot stop for sleep",
            (unsigned)last_rc);
    }

    rc = threadCreate(&g_pm_thread, netSleepThreadMain, NULL, NULL, NET_THREAD_STACK_SIZE,
        NET_THREAD_PRIORITY, -2);

    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_pm_thread);

    if (R_FAILED(rc)) {
        netLog("housekeeping thread rc=0x%08X", (unsigned)rc);
        return;
    }

    if (g_pm_registered)
        netLog("registered with the power state coordinator");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void dglabNetSocketInitialize(void)
{
    if (g_net.mutex_ready)
        return;

    mutexInit(&g_net.mutex);
    g_net.mutex_ready = true;
}

static Result netOpenListenSocket(u16 port, int* out_fd)
{
    struct sockaddr_in addr;
    int fd;
    int enable = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return MAKERESULT(Module_Libnx, LibnxError_IoError);

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (const struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(fd, NET_LISTEN_BACKLOG) != 0) {
        close(fd);
        return MAKERESULT(Module_Libnx, LibnxError_IoError);
    }

    *out_fd = fd;

    return 0;
}

// Creates the server core on first use, with the services it needs. Must be
// called with the lock held.
//
// Note for anything added here: a sysmodule has no filesystem mounted (libnx
// only mounts sdmc for applets), and touching a path without mounting it first
// does not fail cleanly - the 1.7 crash report for this sysmodule was a data
// abort inside mkdir called from exactly this spot.
static void netCoreEnsureReady(u16 port)
{
    DglabNetServerConfig config;
    Result rc;

    if (g_net.core_ready)
        return;

    // The core generates its controller id from the random source, so csrng has
    // to be up first; without it the id falls back to a fixed one.
    if (!g_net.csprng_ready)
        g_net.csprng_ready = R_SUCCEEDED(csrngInitialize());

    memset(&config, 0, sizeof(config));
    config.port = port;
    config.now_ms = netNowMs;
    config.fill_random = netFillRandom;
    config.get_ip = netGetIp;

    dglabNetServerInit(&g_net.server, &config);
    g_net.core_ready = true;

    rc = g_net.csprng_ready ? 0 : MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    if (R_FAILED(rc))
        dglabNetServerLog(&g_net.server, "csrng unavailable, the id is a fallback");
}

Result dglabNetSocketStart(u16 port)
{
    Result rc;
    int fd = -1;

    if (port == 0)
        port = (u16)DGLAB_NET_DEFAULT_PORT;

    dglabNetSocketInitialize();

    mutexLock(&g_net.mutex);

    if (g_net.running) {
        mutexUnlock(&g_net.mutex);
        return 0;
    }

    // nifm is only needed once the server runs: it answers "what is our LAN
    // address" for the QR code.
    if (!g_net.nifm_ready) {
        rc = nifmInitialize(NifmServiceType_User);

        if (R_SUCCEEDED(rc))
            g_net.nifm_ready = true;
        else
            dglabNetServerLog(&g_net.server,
                "nifm unavailable rc=0x%08X, the QR code will have no address", (unsigned)rc);
    }

    if (!g_net.bsd_ready) {
        SocketInitConfig sock = *socketGetDefaultInitConfig();

        // Small buffers: the protocol only carries JSON envelopes. A blocking
        // call holds one session, and these can overlap: two client reads, the
        // accept poll, the heartbeat send, an IPC send and an IPC reply.
        sock.tcp_tx_buf_size = 0x2000;
        sock.tcp_rx_buf_size = 0x2000;
        sock.tcp_tx_buf_max_size = 0x4000;
        sock.tcp_rx_buf_max_size = 0x4000;
        sock.udp_tx_buf_size = 0x800;
        sock.udp_rx_buf_size = 0x1000;
        sock.sb_efficiency = 1;
        sock.num_bsd_sessions = 6;
        sock.bsd_service_type = BsdServiceType_User;

        rc = socketInitialize(&sock);

        if (R_FAILED(rc)) {
            dglabNetServerSetFailed(&g_net.server, rc);
            mutexUnlock(&g_net.mutex);
            return rc;
        }

        g_net.bsd_ready = true;
    }

    netCoreEnsureReady(port);
    g_net.requested_port = port;

    rc = netOpenListenSocket(port, &fd);

    if (R_FAILED(rc)) {
        dglabNetServerLog(&g_net.server, "listen on port %u failed", (unsigned)port);
        dglabNetServerSetFailed(&g_net.server, rc);
        mutexUnlock(&g_net.mutex);
        return rc;
    }

    g_net.listen_fd = fd;
    g_net.stopping = false;
    g_net.last_activity_ms = netNowMs(NULL);
    dglabNetServerSetListening(&g_net.server, port);

    rc = threadCreate(&g_net.accept_thread, netAcceptThreadMain, NULL, NULL, NET_THREAD_STACK_SIZE,
        NET_THREAD_PRIORITY, -2);

    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_net.accept_thread);

    if (R_FAILED(rc)) {
        dglabNetServerLog(&g_net.server, "accept thread failed rc=0x%08X", (unsigned)rc);
        close(g_net.listen_fd);
        g_net.listen_fd = -1;
        dglabNetServerSetFailed(&g_net.server, rc);
        mutexUnlock(&g_net.mutex);
        return rc;
    }

    g_net.accept_valid = true;
    // From here on the server owns threads, so it is "running" for the sake of
    // stop() even if the timer thread below fails.
    g_net.running = true;

    rc = threadCreate(&g_net.tick_thread, netTickThreadMain, NULL, NULL, NET_THREAD_STACK_SIZE,
        NET_THREAD_PRIORITY, -2);

    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_net.tick_thread);

    if (R_FAILED(rc)) {
        dglabNetServerLog(&g_net.server, "timer thread failed rc=0x%08X", (unsigned)rc);
        dglabNetServerSetFailed(&g_net.server, rc);
        // Wake the accept thread so it leaves its loop; a later stop() joins it.
        // The failed Thread handle is deliberately left alone: it was never
        // started, and closing one that cannot be waited for is riskier than
        // leaking a handle slot on an error this serious.
        g_net.stopping = true;
        shutdown(g_net.listen_fd, SHUT_RDWR);
        mutexUnlock(&g_net.mutex);
        return rc;
    }

    g_net.tick_valid = true;

    mutexUnlock(&g_net.mutex);

    return 0;
}

Result dglabNetSocketStop(void)
{
    Thread accept_thread;
    Thread tick_thread;
    Thread client_threads[DGLAB_NET_MAX_CLIENTS];
    bool client_valid[DGLAB_NET_MAX_CLIENTS];
    int client_fds[DGLAB_NET_MAX_CLIENTS];
    bool have_accept = false;
    bool have_tick = false;
    int listen_fd;

    mutexLock(&g_net.mutex);

    if (!g_net.running) {
        mutexUnlock(&g_net.mutex);
        return 0;
    }

    g_net.stopping = true;

    if (g_net.accept_valid) {
        accept_thread = g_net.accept_thread;
        g_net.accept_valid = false;
        have_accept = true;
    }

    if (g_net.tick_valid) {
        tick_thread = g_net.tick_thread;
        g_net.tick_valid = false;
        have_tick = true;
    }

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        client_valid[i] = g_net.clients[i].thread_valid;
        client_threads[i] = g_net.clients[i].thread;
        client_fds[i] = g_net.clients[i].fd;
    }

    listen_fd = g_net.listen_fd;

    // Say goodbye properly before the sockets go: a plain shutdown leaves the
    // App with a dead TCP connection it does not always notice.
    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (g_net.clients[i].active && g_net.clients[i].conn.handshake_done)
            wsConnSend(&g_net.clients[i].conn, WsOpcode_Close, NULL, 0);
    }

    mutexUnlock(&g_net.mutex);

    // shutdown() wakes the blocking accept()/recv() the threads are sitting in.
    if (listen_fd >= 0)
        shutdown(listen_fd, SHUT_RDWR);

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (client_valid[i] && client_fds[i] >= 0)
            shutdown(client_fds[i], SHUT_RDWR);
    }

    if (have_accept) {
        threadWaitForExit(&accept_thread);
        threadClose(&accept_thread);
    }

    if (have_tick) {
        threadWaitForExit(&tick_thread);
        threadClose(&tick_thread);
    }

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (!client_valid[i])
            continue;

        // The client thread closes its own socket, so only the handle is freed.
        threadWaitForExit(&client_threads[i]);
        threadClose(&client_threads[i]);
    }

    mutexLock(&g_net.mutex);

    if (listen_fd >= 0)
        close(listen_fd);

    g_net.listen_fd = -1;

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        memset(&g_net.clients[i].conn, 0, sizeof(g_net.clients[i].conn));
        g_net.clients[i].fd = -1;
        g_net.clients[i].active = false;
        g_net.clients[i].thread_valid = false;
    }

    dglabNetServerSetStopped(&g_net.server);

    g_net.running = false;
    g_net.stopping = false;

    mutexUnlock(&g_net.mutex);

    return 0;
}

Result dglabNetSocketGetStatus(DglabNetStatus* out)
{
    if (!out)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    mutexLock(&g_net.mutex);
    dglabNetServerGetStatus(&g_net.server, out);
    mutexUnlock(&g_net.mutex);

    return 0;
}

Result dglabNetSocketGetQr(char* out, size_t out_size, size_t* out_written)
{
    bool ok;

    if (!out || out_size == 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    mutexLock(&g_net.mutex);
    ok = dglabNetServerGetQr(&g_net.server, out, out_size, out_written);
    mutexUnlock(&g_net.mutex);

    return ok ? 0 : MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

Result dglabNetSocketSend(const DglabNetSendRequest* request)
{
    DglabNetSendResult result;

    if (!request)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    mutexLock(&g_net.mutex);
    result = dglabNetServerSend(&g_net.server, request);
    mutexUnlock(&g_net.mutex);

    switch (result) {
        case DglabNetSend_Ok:
            return 0;
        case DglabNetSend_NotPaired:
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);
        case DglabNetSend_IoError:
            return MAKERESULT(Module_Libnx, LibnxError_IoError);
        case DglabNetSend_BadRequest:
        case DglabNetSend_TooLong:
        default:
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
}

u32 dglabNetSocketReadLog(u32 cursor, char* out, size_t out_size)
{
    u32 next;

    if (!out || out_size == 0)
        return cursor;

    mutexLock(&g_net.mutex);
    next = dglabNetServerReadLog(&g_net.server, cursor, out, out_size);
    mutexUnlock(&g_net.mutex);

    return next;
}
