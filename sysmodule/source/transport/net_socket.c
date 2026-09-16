#include <dglab/transport/net_socket.h>

#include <dglab/build.h>
#include <dglab/net/net_server.h>

#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
    bool csprng_ready;
    bool running;
    u16 requested_port; // port to bring back up after a sleep
    bool restart_after_sleep;
    uint64_t last_activity_ms;
    bool idle_stop_pending; // set by the tick thread, applied by the IPC thread
    bool sd_log_enabled;    // only ever true while the server runs
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

// The in-memory log ring dies with the process, and a hang takes the NRO with
// it, so once the server runs the sysmodule also writes its log to the SD card.
// Nothing here may run at boot: a sysmodule has no filesystem mounted then, and
// touching a path crashed the console at the logo once already.
#define NET_SD_LOG_DIR "sdmc:/switch/DGLAB-NX/logs"
#define NET_SD_LOG_PATH NET_SD_LOG_DIR "/dglab-sys.log"

static FILE* g_sd_log;
static bool g_sd_log_failed;

static void netSdLog(void* context, const char* line)
{
    (void)context;

    if (!g_net.sd_log_enabled || g_sd_log_failed)
        return;

    if (g_sd_log == NULL) {
        // Both calls are ignored on purpose: whichever part is already up (fs or
        // the sdmc mount) simply reports "already initialised", and the fopen
        // below is the real test of whether the path is usable.
        fsInitialize();
        fsdevMountSdmc();

        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/DGLAB-NX", 0777);
        mkdir(NET_SD_LOG_DIR, 0777);
        g_sd_log = fopen(NET_SD_LOG_PATH, "w");

        if (g_sd_log == NULL) {
            g_sd_log_failed = true;
            return;
        }
    }

    fprintf(g_sd_log, "%s\n", line);
    fflush(g_sd_log);
}

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
    // nifm is opened for the query and closed again: this sysmodule has no
    // business holding a network session while the console is idle, and that is
    // a candidate for what keeps a console from sleeping. The answer is cached
    // for a couple of seconds because the NRO polls the status every frame.
    static u32 cached_ip;
    static char cached_text[16];
    static bool cached_valid;
    static uint64_t cached_at_ms;
    u32 ip = 0;
    Result rc;
    uint64_t now_ms;

    (void)context;

    now_ms = netNowMs(NULL);

    if (cached_valid && now_ms - cached_at_ms < 2000u) {
        snprintf(text, text_size, "%s", cached_text);
        *address = cached_ip;

        return cached_ip != 0;
    }

    rc = nifmInitialize(NifmServiceType_User);

    if (R_FAILED(rc)) {
        cached_valid = true;
        cached_at_ms = now_ms;
        cached_ip = 0;
        cached_text[0] = '\0';

        return false;
    }

    rc = nifmGetCurrentIpAddress(&ip);
    nifmExit();

    if (R_FAILED(rc) || ip == 0) {
        cached_valid = true;
        cached_at_ms = now_ms;
        cached_ip = 0;
        cached_text[0] = '\0';

        return false;
    }

    // nifm reports the address as struct in_addr, so the bytes are already in
    // the order a dotted quad needs.
    const u8* bytes = (const u8*)&ip;

    snprintf(text, text_size, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    snprintf(cached_text, sizeof(cached_text), "%s", text);

    cached_ip = ip;
    cached_valid = true;
    cached_at_ms = now_ms;
    *address = ip;

    return true;
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

// The one place the transport's own lines are formatted. Callers hold no lock;
// this takes the transport lock, which is why nothing may call it from inside a
// critical section (the core's dglabNetServerLog is the one to use there).
static void netLogV(const char* fmt, va_list args)
{
    char line[192];

    vsnprintf(line, sizeof(line), fmt, args);

    mutexLock(&g_net.mutex);
    dglabNetServerLog(&g_net.server, "%s", line);
    mutexUnlock(&g_net.mutex);
}

static void netLog(const char* fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    netLogV(fmt, args);
    va_end(args);
}

void dglabNetSocketLogNote(const char* fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    netLogV(fmt, args);
    va_end(args);
}

// Creates the server core on first use (defined below, next to the public API).
static void netCoreEnsureReady(u16 port);
// Marks an idle server for shutdown (defined with the sleep handling).
static void netCheckIdle(void);

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
            uint32_t pings_before = conn->ping_count;

            if (!wsConnRecv(conn, &opcode, payload, sizeof(payload), &size))
                break;

            // WebSocket pings are handled inside the frame layer, so they are
            // counted here: the App keeps the link alive with them and would
            // otherwise look completely silent.
            if (conn->ping_count != pings_before) {
                mutexLock(&g_net.mutex);

                if (conn->ping_count <= 4)
                    dglabNetServerLog(&g_net.server, "rx ping #%u", (unsigned)conn->ping_count);

                dglabNetServerOnActivity(&g_net.server, conn);
                mutexUnlock(&g_net.mutex);
            }

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

        netCheckIdle();

        mutexLock(&g_net.mutex);
        dglabNetServerPoll(&g_net.server, netNowMs(NULL));
        mutexUnlock(&g_net.mutex);
    }
}

// ---------------------------------------------------------------------------
// Sleep handling
//
// Holding sockets across a system sleep is what hangs a console. Everything in
// this section therefore happens while the server runs, never at boot: an
// earlier revision did it at boot and the console stopped booting at the logo.
//
// The power state watch is registered by dglabNetSocketStart(). Its thread lives
// for the whole process, so it never has to be joined from inside itself - which
// matters, because the thread has to tear the server down before it acknowledges
// ReadySleep.
// ---------------------------------------------------------------------------

static PscPmModule g_pm_module;
static bool g_pm_registered;
static bool g_pm_attempted;
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

static void netPmThreadMain(void* arg)
{
    (void)arg;

    for (;;) {
        PscPmState state;
        u32 flags;

        // The event is not autoclear; the timeout only keeps the loop ticking.
        eventWait(&g_pm_module.event, 500000000ull);

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

// Registers the power state watch and starts its thread. Called by
// dglabNetSocketStart, at most once per process; failures are logged and the
// server simply keeps running.
static void netPmStart(void)
{
    static const u32 dependencies[] = { PscPmModuleId_WlanSockets };
    // Only this one id is ever attempted, and only after the user started the
    // server. Requesting any other id freezes the whole console: the ids 200 and
    // 201 did it once at boot (stuck on the logo) and once on the A press (every
    // button dead until the power button). This attempt is safe by contrast: the
    // system owns the id, the call returns 0x0000108A immediately, and it has
    // been through several hardware runs.
    static const PscPmModuleId kCandidates[] = {
        PscPmModuleId_WlanSockets,
    };
    Result rc;
    Result last_rc = 0;
    size_t candidate;

    if (g_pm_attempted)
        return;

    g_pm_attempted = true;

    rc = pscmInitialize();

    if (R_SUCCEEDED(rc)) {
        for (candidate = 0; candidate < sizeof(kCandidates) / sizeof(kCandidates[0]); candidate++) {
            rc = pscmGetPmModule(&g_pm_module, kCandidates[candidate], dependencies, 1, false);

            if (R_SUCCEEDED(rc))
                break;

            last_rc = rc;
        }
    } else {
        last_rc = rc;
    }

    if (R_FAILED(rc)) {
        // The server still runs; it just cannot be told to step aside for sleep.
        netLog("sleep watch unavailable rc=0x%08X, the server cannot stop for sleep",
            (unsigned)last_rc);
        return;
    }

    g_pm_registered = true;

    rc = threadCreate(&g_pm_thread, netPmThreadMain, NULL, NULL, NET_THREAD_STACK_SIZE,
        NET_THREAD_PRIORITY, -2);

    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_pm_thread);

    if (R_FAILED(rc)) {
        netLog("sleep watch thread rc=0x%08X", (unsigned)rc);
        g_pm_registered = false;
        pscPmModuleClose(&g_pm_module);
        return;
    }

    netLog("sleep watch registered as module %u", (unsigned)kCandidates[candidate]);
}

// A server nobody connected to for a while puts itself away: holding the
// listening socket across a sleep is what hangs the console, and there is no
// working sleep notification. The window is shorter than the shortest auto-sleep
// the console offers (60s), so a console left alone always loses the socket
// before it can try to sleep. The tick thread only marks the request; the stop
// itself happens on the next IPC call, because a server thread must not join
// itself.
#define NET_IDLE_STOP_MS (55u * 1000u)

static void netCheckIdle(void)
{
    uint64_t now_ms = netNowMs(NULL);

    mutexLock(&g_net.mutex);

    if (g_net.running) {
        if (g_net.server.status.clients > 0) {
            // A live connection is a sign of life, so the timer only runs from
            // the moment the last client went away.
            g_net.last_activity_ms = now_ms;
        } else if (g_net.last_activity_ms != 0 &&
                   now_ms - g_net.last_activity_ms >= NET_IDLE_STOP_MS) {
            g_net.idle_stop_pending = true;
        }
    }

    mutexUnlock(&g_net.mutex);
}

static void netApplyPendingStop(void)
{
    bool pending;

    mutexLock(&g_net.mutex);
    pending = g_net.idle_stop_pending;
    g_net.idle_stop_pending = false;
    mutexUnlock(&g_net.mutex);

    if (!pending)
        return;

    netLog("no client for %u s, stopping the server before the console can sleep",
        (unsigned)(NET_IDLE_STOP_MS / 1000u));
    dglabNetSocketStop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void dglabNetSocketInitialize(void)
{
    if (!g_net.mutex_ready) {
        mutexInit(&g_net.mutex);
        g_net.mutex_ready = true;
    }

    // Boot does only this: the session core and its log ring. Threads, sockets,
    // services and the power state watch all wait for dglabNetSocketStart().
    mutexLock(&g_net.mutex);
    netCoreEnsureReady((u16)DGLAB_NET_DEFAULT_PORT);
    mutexUnlock(&g_net.mutex);
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
    config.log_sink = netSdLog;

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

    netApplyPendingStop();

    if (port == 0)
        port = (u16)DGLAB_NET_DEFAULT_PORT;

    dglabNetSocketInitialize();

    mutexLock(&g_net.mutex);

    if (g_net.running) {
        mutexUnlock(&g_net.mutex);
        return 0;
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
    g_net.sd_log_enabled = true; // from here on the log also lands on the SD card
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

    // The SD mirror is on from here, so this one line ends up at the top of
    // dglab-sys.log: whoever reads that file can tell which build wrote it.
    dglabNetSocketLogNote("server start, dglab %s", DGLAB_BUILD_STAMP);

    // The power state watch needs its own IPC and a thread, so it is started
    // here rather than at boot: the watch only matters while the server runs.
    netPmStart();

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

    if (g_sd_log != NULL) {
        fclose(g_sd_log);
        g_sd_log = NULL;
    }

    g_net.sd_log_enabled = false;
    g_net.running = false;
    g_net.stopping = false;

    mutexUnlock(&g_net.mutex);

    return 0;
}

Result dglabNetSocketGetStatus(DglabNetStatus* out)
{
    if (!out)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    netApplyPendingStop();

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

    netApplyPendingStop();

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

    netApplyPendingStop();

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

Result dglabNetSocketUploadWaveform(const DglabNetWaveformRequest* request)
{
    DglabNetSendResult result;

    if (!request)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    netApplyPendingStop();

    mutexLock(&g_net.mutex);
    result = dglabNetServerUploadWaveform(&g_net.server, request);
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

    netApplyPendingStop();

    mutexLock(&g_net.mutex);
    next = dglabNetServerReadLog(&g_net.server, cursor, out, out_size);
    mutexUnlock(&g_net.mutex);

    return next;
}
