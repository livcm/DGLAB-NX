#include <dglab/transport/net_socket.h>

#include <dglab/build.h>
#include <dglab/net/net_server.h>

#include <errno.h>
#include <malloc.h>
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
// Stall safety net: a stage marker plus a watchdog that reports and tries to
// break a send() loose once the marker stops moving. See netStage() /
// netWatchdogThreadMain(). It is what turned "must reboot the console" into
// "stalls for a few seconds".
#define NET_STALL_PATH "sdmc:/switch/DGLAB-NX/logs/dglab-stall.log"
#define NET_STALL_POLL_MS 500u
#define NET_STALL_MS 3000u

// Usable stack per thread. libnx's threadCreate() also has to fit the thread's
// TLS block and reent struct above it, and when it is not handed a stack it takes
// the whole thing from the heap with aligned_alloc(0x1000, ...) - a heap that is
// this sysmodule's fixed 512KB inner array (INNER_HEAP_SIZE in
// sysmodule/source/main.c). These threads are created and released on every
// start/stop round, so they are given static stacks instead and the heap stays out
// of thread creation entirely; the thread lifecycle bug that made those rounds
// leak is described in docs/dglab-socket.md, "反复启停后服务端起不来".
#define NET_THREAD_STACK_SIZE 0x4000u
// Page aligned total handed to threadCreate(): the usable stack above plus the
// TLS and reent underneath (rounded up; libnx wants 0x30 of slack on top of
// them). It rejects a stack that is not page aligned, or one too small to hold
// TLS + reent + that slack.
#define NET_THREAD_STACK_TOTAL 0x5000u
#define NET_THREAD_PRIORITY 0x2C
#define NET_TICK_INTERVAL_MS 100
#define NET_TICK_INTERVAL_NS (NET_TICK_INTERVAL_MS * 1000000ull)

_Static_assert((NET_THREAD_STACK_TOTAL & 0xFFFu) == 0,
    "threadCreate() requires a page aligned stack");
_Static_assert(NET_THREAD_STACK_TOTAL >= NET_THREAD_STACK_SIZE + 0x800u,
    "NET_THREAD_STACK_TOTAL must leave NET_THREAD_STACK_SIZE usable after TLS + reent");

// One static stack per thread, .bss like the frames in net_server.c: the accept,
// tick and power-state threads are created again on every start, so the heap must
// not be involved in creating a thread at all.
static u8 g_accept_thread_stack[NET_THREAD_STACK_TOTAL] __attribute__((aligned(0x1000)));
static u8 g_tick_thread_stack[NET_THREAD_STACK_TOTAL] __attribute__((aligned(0x1000)));
static u8 g_pm_thread_stack[NET_THREAD_STACK_TOTAL] __attribute__((aligned(0x1000)));
// Each row is a whole number of pages, so the array's alignment is every slot's.
static u8 g_client_thread_stack[DGLAB_NET_MAX_CLIENTS][NET_THREAD_STACK_TOTAL]
    __attribute__((aligned(0x1000)));

// Stall safety net: where each thread currently is. Plain stores only - no locks,
// no file system - because added I/O in the hot path hid this bug twice already.
// The watchdog below reports it once it stops moving.
typedef enum {
    NetStage_None = 0,
    NetStage_Start,
    NetStage_Tick,
    NetStage_SendBegin,
    NetStage_SendEnd,
    NetStage_LogBegin,
    NetStage_LogEnd,
} NetStage;

typedef enum {
    NetStageThread_Ipc = 0,
    NetStageThread_Tick,
    NetStageThread_Accept,
    NetStageThread_Client,
    NetStageThread_Watchdog,
} NetStageThread;

static volatile int g_stage = NetStage_None;
static volatile uint64_t g_stage_ms;
static volatile uint32_t g_stage_count;
static volatile int g_stage_thread;
static volatile int g_stage_fd = -1;
static volatile uint32_t g_stage_bytes;
static volatile int g_stage_errno;

static u8 g_watchdog_thread_stack[NET_THREAD_STACK_TOTAL] __attribute__((aligned(0x1000)));
static Thread g_watchdog_thread;
static bool g_watchdog_started;
static FILE* g_stall_log;
static bool g_stall_log_failed;

typedef struct {
    int fd; ///< -1 while the slot is unused
    Thread thread;
    bool thread_valid; ///< a handle exists and has to be joined before reuse
    bool active;       ///< a running connection owns the slot
    Mutex write_mutex; ///< serialises frames from the core and from wsConnRecv
    bool write_mutex_ready;
    // Frames the core produced while holding the transport lock. They are
    // memcpy'd here (never written from under that lock) and flushed right after
    // the lock is released, so a bsd:u send that never returns can only park the
    // thread that flushes - not the IPC thread and not the console's NRO.
    uint8_t tx[4096];
    size_t tx_pending;
    // A failed write is reported by the tick thread, never from inside the write
    // itself: the connection thread reaches this while holding the frame lock,
    // and taking the transport lock there inverts the order the IPC and tick
    // threads use (transport -> frame) and deadlocks both of them.
    bool write_fail_pending; ///< the tick thread still has to log it
    int write_fail_errno;
    int write_fail_left;
    int write_fail_fd;
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

// Opens a file under sdmc:/switch/DGLAB-NX/logs/, mounting what has to be mounted
// first. Both calls are ignored on purpose: whichever part is already up (fs or
// the sdmc mount) simply reports "already initialised", and the fopen below is
// the real test of whether the path is usable.
static FILE* netSdOpen(const char* path, const char* mode)
{
    fsInitialize();
    fsdevMountSdmc();

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/DGLAB-NX", 0777);
    mkdir(NET_SD_LOG_DIR, 0777);

    return fopen(path, mode);
}

// Stall safety net: the stage marker. Everything here is a plain store, so it can
// run inside any of the hot paths without changing their timing. (Both are defined
// further down; the watchdog needs them first.)
static uint64_t netNowMs(void* context);
static void netLog(const char* fmt, ...);

static int netStageThreadNow(void)
{
    Handle self = threadGetCurHandle();

    if (self == g_net.tick_thread.handle)
        return NetStageThread_Tick;

    if (self == g_net.accept_thread.handle)
        return NetStageThread_Accept;

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (g_net.clients[i].thread_valid && self == g_net.clients[i].thread.handle)
            return NetStageThread_Client;
    }

    return NetStageThread_Ipc;
}

static void netStage(int stage, int thread, int fd, uint32_t bytes, int err)
{
    g_stage_thread = thread;
    g_stage_fd = fd;
    g_stage_bytes = bytes;
    g_stage_errno = err;
    g_stage_ms = netNowMs(NULL);
    g_stage = stage;
    g_stage_count++;
}

static void netSdLog(void* context, const char* line)
{
    (void)context;

    if (!g_net.sd_log_enabled || g_sd_log_failed)
        return;

    if (g_sd_log == NULL) {
        g_sd_log = netSdOpen(NET_SD_LOG_PATH, "w");

        if (g_sd_log == NULL) {
            g_sd_log_failed = true;
            return;
        }
    }

    netStage(NetStage_LogBegin, netStageThreadNow(), -1, (uint32_t)strlen(line), 0);

    fprintf(g_sd_log, "%s\n", line);
    fflush(g_sd_log);

    netStage(NetStage_LogEnd, netStageThreadNow(), -1, 0, 0);
}

static const char* netStageName(int stage)
{
    switch (stage) {
        case NetStage_Start: return "server start";
        case NetStage_Tick: return "tick loop";
        case NetStage_SendBegin: return "send begin";
        case NetStage_SendEnd: return "send end";
        case NetStage_LogBegin: return "log write begin";
        case NetStage_LogEnd: return "log write end";
        default: return "none";
    }
}

static const char* netStageThreadName(int thread)
{
    switch (thread) {
        case NetStageThread_Tick: return "tick";
        case NetStageThread_Accept: return "accept";
        case NetStageThread_Client: return "client";
        case NetStageThread_Watchdog: return "watchdog";
        default: return "ipc";
    }
}

// Reports the stage marker once it has not moved for NET_STALL_MS, and shuts the
// stalled socket down to break the caller loose. It only writes after the stall
// has already happened, so it cannot hide the bug. If dglab-stall.log is missing
// or empty after a hang, the stall is in the file system path itself (this thread
// could not write either).
static void netWatchdogThreadMain(void* arg)
{
    uint64_t last_count = 0;
    uint64_t last_change_ms = 0;
    bool reported = true; // nothing to report until the marker moved once

    (void)arg;

    for (;;) {
        uint64_t now_ms;

        svcSleepThread((u64)NET_STALL_POLL_MS * 1000000ull);

        now_ms = netNowMs(NULL);

        if (g_stage_count != last_count) {
            last_count = g_stage_count;
            last_change_ms = now_ms;
            reported = false;
            continue;
        }

        if (reported || now_ms - last_change_ms < NET_STALL_MS)
            continue;

        reported = true;

        if (g_stall_log == NULL && !g_stall_log_failed) {
            g_stall_log = netSdOpen(NET_STALL_PATH, "a");

            if (g_stall_log == NULL)
                g_stall_log_failed = true;
        }

        if (g_stall_log != NULL) {
            fprintf(g_stall_log,
                "stall: %s for %u ms, thread=%s, fd=%d, bytes=%u, errno=%d, "
                "clients=%u, state=%u\n",
                netStageName(g_stage), (unsigned)(now_ms - g_stage_ms),
                netStageThreadName(g_stage_thread), g_stage_fd, (unsigned)g_stage_bytes,
                g_stage_errno, (unsigned)g_net.server.status.clients,
                (unsigned)g_net.server.status.state);

            // The stage says a send() never came back: shut the socket down from
            // here to break the caller loose (hardware confirmed 2026-09-19 that
            // this recovers instead of freezing the sysmodule for good).
            if (g_stage == NetStage_SendBegin && g_stage_fd >= 0) {
                fprintf(g_stall_log, "stall: shutting down fd %d\n", g_stage_fd);
                shutdown(g_stage_fd, SHUT_RDWR);
            }

            fflush(g_stall_log);
        }
    }
}

// Created with the first successful start and deliberately never joined.
static void netWatchdogEnsureStarted(void)
{
    Result rc;

    if (g_watchdog_started)
        return;

    rc = threadCreate(&g_watchdog_thread, netWatchdogThreadMain, NULL, g_watchdog_thread_stack,
        sizeof(g_watchdog_thread_stack), NET_THREAD_PRIORITY, -2);

    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_watchdog_thread);

    if (R_FAILED(rc)) {
        netLog("watchdog thread failed rc=0x%08X", (unsigned)rc);
        return;
    }

    g_watchdog_started = true;
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

// The transport's heap probe. The sysmodule's heap is a fixed 512KB array, so a
// retained allocation never shows up as a crash - it shows up as a later start
// failing to create a thread. One line per start and per stop makes that visible
// in dglab-sys.log and in the ring the NRO copies to dglab-net.log; the delta is
// against the same phase of the previous cycle, which is the number that has to
// stay flat.
//
// Which lock to hold follows netLog and dglabNetServerLog: the start path calls
// this with no lock held, the stop path calls it while the transport lock is
// still held.
//
// newlib's mallinfo() walks the arena without taking its own malloc lock, so it
// is only called where nothing else can be allocating: on start before a client
// can exist, on stop after the server's threads have been joined.
static bool netFormatHeap(char* out, size_t out_size, bool starting)
{
    struct mallinfo info = mallinfo();
    static unsigned last_start_used_kb;
    static unsigned last_stop_used_kb;
    static bool start_valid;
    static bool stop_valid;
    unsigned used_kb = (unsigned)(info.uordblks / 1024);
    unsigned free_kb = (unsigned)(info.fordblks / 1024);
    unsigned arena_kb = (unsigned)(info.arena / 1024);
    unsigned* previous = starting ? &last_start_used_kb : &last_stop_used_kb;
    bool* valid = starting ? &start_valid : &stop_valid;
    long delta_kb = *valid ? (long)used_kb - (long)*previous : 0;

    *previous = used_kb;
    *valid = true;

    return snprintf(out, out_size, "heap %s: used=%uk free=%uk arena=%uk (delta %+ldk)",
               starting ? "start" : "stop", used_kb, free_kb, arena_kb, delta_kb) > 0;
}

static void netLogHeap(bool starting)
{
    char line[96];

    if (netFormatHeap(line, sizeof(line), starting))
        netLog("%s", line);
}

static void netLogHeapLocked(bool starting)
{
    char line[96];

    if (netFormatHeap(line, sizeof(line), starting))
        dglabNetServerLog(&g_net.server, "%s", line);
}

// Waits for one thread that has already been asked to leave and releases it.
//
// The Thread handed in has to be the live one, never a copy taken before the
// wait: libnx's threadClose() refuses a Thread that is still in its thread list
// (tls_array, offset 32, which _EntryWrap sets when the thread starts and
// threadExit clears when it ends) and returns LibnxError_BadInput (0x1759)
// without releasing anything. A copy still carries the value from the moment it
// was taken - while the thread was running - so closing one silently leaked the
// thread's stack, mirror mapping and handle on every single start/stop round,
// which is what made the server refuse to start (docs/dglab-socket.md, "反复启停
// 后服务端起不来").
//
// The results are logged rather than dropped, for the same reason. Callers hold
// no lock (netLog takes it).
static void netJoinThread(const char* what, Thread* thread)
{
    Result rc = threadWaitForExit(thread);
    Result close_rc;

    if (R_FAILED(rc))
        netLog("thread %s did not exit, rc=0x%08X", what, (unsigned)rc);

    close_rc = threadClose(thread);

    if (R_FAILED(close_rc))
        netLog("thread close (%s) rc=0x%08X (the thread's stack was not released)",
            what, (unsigned)close_rc);
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

// The frame lock the frame layer uses around "build the frame, write it once".
// It used to sit inside netSocketWrite(), which meant the header and the payload
// of one frame were two separately locked writes: another sender could slip a
// frame in between them, and a failure could leave half a frame on the wire.
static void netWriteLock(void* context)
{
    NetSocketClient* slot = context;

    mutexLock(&slot->write_mutex);
}

static void netWriteUnlock(void* context)
{
    NetSocketClient* slot = context;

    mutexUnlock(&slot->write_mutex);
}

// The actual syscall path. The frame lock is already held here (by the frame
// layer, or by netFlushPending), never the transport lock.
static int netWriteNow(NetSocketClient* slot, const uint8_t* buffer, size_t size)
{
    int fd = slot->fd;
    size_t written = 0;
    int result;

    // The frame lock is held by the caller (WS layer) for the whole frame. A
    // failure is logged once per connection and also drops it: a socket that
    // cannot take one whole frame has a peer that is not keeping up, and the
    // server would rather lose that peer than park a thread inside send().

    while (written < size) {
        // Blocking, bounded by the socket's SO_SNDTIMEO (5 s). Not MSG_DONTWAIT:
        // a 2026-09-19 hardware run (see docs/dglab-socket.md, "socket 写路径")
        // showed a bsd:u send that never came back with that flag set - the caller
        // parks in the kernel, which the flag cannot prevent. Only a frame header
        // or one payload goes through here, and the waveform batches are sent by
        // the tick thread, so a slow peer cannot park the IPC thread either.
        ssize_t rc;
        int err;

        netStage(NetStage_SendBegin, netStageThreadNow(), fd, (uint32_t)(size - written), 0);

        rc = send(fd, buffer + written, size - written, MSG_NOSIGNAL);
        err = errno;

        netStage(NetStage_SendEnd, netStageThreadNow(), fd, (uint32_t)(size - written), err);

        if (rc < 0 && err == EINTR)
            continue;

        if (rc <= 0 || (size_t)rc < size - written) {
            // Recorded, not logged: this runs with the frame lock held, and the
            // transport lock is the wrong one to take from here (netTickThreadMain
            // prints it on its next pass). Callers are serialised by the frame
            // lock, so writing these fields needs no lock of its own.
            if (!slot->write_fail_pending) {
                slot->write_fail_pending = true;
                slot->write_fail_errno = err;
                slot->write_fail_left = (int)(size - written);
                slot->write_fail_fd = fd;
            }

            shutdown(fd, SHUT_RDWR);

            return -1;
        }

        written += (size_t)rc;
    }

    result = (int)size;

    return result;
}

// The WS layer's write callback. When the caller holds the transport lock the
// bytes are only copied into the slot's queue: doing the socket call from under
// that lock is what let a stalled bsd:u send freeze the whole sysmodule (the IPC
// thread waited for the lock). The copy is done under the frame lock, and the
// queue is flushed by netFlushPending() once the transport lock is gone.
static int netSocketWrite(void* context, const uint8_t* buffer, size_t size)
{
    NetSocketClient* slot = context;

    if (!mutexIsLockedByCurrentThread(&g_net.mutex))
        return netWriteNow(slot, buffer, size);

    if (size > sizeof(slot->tx) - slot->tx_pending) {
        // Cannot happen with one frame per send; dropping is better than writing
        // from here. The line is safe: the caller holds the transport lock.
        dglabNetServerLog(&g_net.server, "tx queue full, dropped %u bytes", (unsigned)size);
        return (int)size;
    }

    memcpy(slot->tx + slot->tx_pending, buffer, size);
    slot->tx_pending += size;

    return (int)size;
}

// Writes what the core queued. Called without the transport lock, so a send that
// never returns parks only the calling thread.
static void netFlushPending(NetSocketClient* slot)
{
    size_t offset = 0;

    if (slot->tx_pending == 0)
        return;

    mutexLock(&slot->write_mutex);

    while (offset < slot->tx_pending) {
        int rc = netWriteNow(slot, slot->tx + offset, slot->tx_pending - offset);

        if (rc <= 0)
            break;

        offset += (size_t)rc;
    }

    slot->tx_pending = 0;

    mutexUnlock(&slot->write_mutex);
}

static void netFlushAllPending(void)
{
    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++)
        netFlushPending(&g_net.clients[i]);
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
        slot->thread_valid = false;
        join_previous = true;
    }

    mutexUnlock(&g_net.mutex);

    // Join before touching the slot: the previous thread clears the slot as its
    // last step, and it must not clear the fd this connection is about to use.
    // netJoinThread() works on the slot's own Thread: closing a copy taken here
    // would be refused by libnx and leave that thread's stack behind.
    if (join_previous)
        netJoinThread("client", &slot->thread);

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
    // The frame layer builds and writes a whole frame under this lock.
    slot->conn.lock = netWriteLock;
    slot->conn.unlock = netWriteUnlock;
    slot->fd = fd;
    slot->active = true;
    slot->write_fail_pending = false;
    slot->tx_pending = 0;

    if (!slot->write_mutex_ready) {
        mutexInit(&slot->write_mutex);
        slot->write_mutex_ready = true;
    }

    rc = threadCreate(&slot->thread, netClientThreadMain, (void*)(uintptr_t)index,
        g_client_thread_stack[index], sizeof(g_client_thread_stack[index]),
        NET_THREAD_PRIORITY, -2);

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

        netStage(NetStage_Tick, NetStageThread_Tick, -1, 0, 0);

        netCheckIdle();

        mutexLock(&g_net.mutex);

        // Write failures arrive here instead of being logged where they happen:
        // the connection thread reports them while holding the frame lock, and
        // taking the transport lock from there would deadlock against this very
        // thread (transport -> frame is the only allowed order).
        for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
            NetSocketClient* slot = &g_net.clients[i];

            if (!slot->write_fail_pending)
                continue;

            slot->write_fail_pending = false;

            dglabNetServerLog(&g_net.server, "write failed: %d bytes left, errno %d, fd %d",
                slot->write_fail_left, slot->write_fail_errno, slot->write_fail_fd);
        }

        dglabNetServerPoll(&g_net.server, netNowMs(NULL));
        mutexUnlock(&g_net.mutex);

        // Outside the transport lock: a send that never returns parks this thread
        // only, and the IPC thread (the NRO) keeps working.
        netFlushAllPending();
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

    rc = threadCreate(&g_pm_thread, netPmThreadMain, NULL, g_pm_thread_stack,
        sizeof(g_pm_thread_stack), NET_THREAD_PRIORITY, -2);

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

    rc = threadCreate(&g_net.accept_thread, netAcceptThreadMain, NULL, g_accept_thread_stack,
        sizeof(g_accept_thread_stack), NET_THREAD_PRIORITY, -2);

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

    rc = threadCreate(&g_net.tick_thread, netTickThreadMain, NULL, g_tick_thread_stack,
        sizeof(g_tick_thread_stack), NET_THREAD_PRIORITY, -2);

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

    netStage(NetStage_Start, NetStageThread_Ipc, -1, 0, 0);

    // Everything the server needs is up (socket, accept thread, tick thread), so
    // this is the figure to compare with the previous start: it has to stay flat
    // however often the user presses A.
    netLogHeap(true);

    // Stall safety net: the watchdog writes nothing until the stage marker stops
    // moving, so it cannot hide the stall it exists to catch.
    netWatchdogEnsureStarted();

    // The power state watch needs its own IPC and a thread, so it is started
    // here rather than at boot: the watch only matters while the server runs.
    netPmStart();

    return 0;
}

Result dglabNetSocketStop(void)
{
    // Only the decisions are snapshotted: which threads exist and which fds to
    // wake. The Thread structs themselves are always used live, never copied -
    // libnx refuses to release a thread that has not finished unregistering
    // itself yet, and only the live struct carries that state (netJoinThread).
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
        g_net.accept_valid = false;
        have_accept = true;
    }

    if (g_net.tick_valid) {
        g_net.tick_valid = false;
        have_tick = true;
    }

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        client_valid[i] = g_net.clients[i].thread_valid;
        client_fds[i] = g_net.clients[i].fd;
    }

    listen_fd = g_net.listen_fd;

    // Say goodbye properly before the sockets go: a plain shutdown leaves the
    // App with a dead TCP connection it does not always notice.
    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (g_net.clients[i].active && g_net.clients[i].conn.handshake_done) {
            // Whatever was still queued for the App is dropped: only the close
            // frame has to get out, so the flush below stays a few bytes.
            g_net.clients[i].tx_pending = 0;
            wsConnSend(&g_net.clients[i].conn, WsOpcode_Close, NULL, 0);
        }
    }

    mutexUnlock(&g_net.mutex);

    // Outside the transport lock, so the App gets its close frame and the rest of
    // the shutdown does not wait for anything else to go out.
    netFlushAllPending();

    // shutdown() wakes the blocking accept()/recv() the threads are sitting in.
    if (listen_fd >= 0)
        shutdown(listen_fd, SHUT_RDWR);

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (client_valid[i] && client_fds[i] >= 0)
            shutdown(client_fds[i], SHUT_RDWR);
    }

    if (have_accept)
        netJoinThread("accept", &g_net.accept_thread);

    if (have_tick)
        netJoinThread("tick", &g_net.tick_thread);

    for (size_t i = 0; i < DGLAB_NET_MAX_CLIENTS; i++) {
        if (!client_valid[i])
            continue;

        // The client thread closes its own socket, so only the handle is freed.
        netJoinThread("client", &g_net.clients[i].thread);
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
        // Dropped, not flushed: the socket is going away, and a queued frame that
        // cannot be written must not keep the stop path waiting.
        g_net.clients[i].tx_pending = 0;
    }

    dglabNetServerSetStopped(&g_net.server);

    // Logged while the SD mirror is still open, hence the locked variant: the
    // stop figure is the one that says whether the threads handed their memory
    // back, and it has to reach dglab-sys.log as well as the ring.
    netLogHeapLocked(false);

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

    netFlushAllPending();

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

    netFlushAllPending();

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
