#include "TailscaleSocket.h"

#ifdef MESHTASTIC_TAILSCALE

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef ARCH_ESP32
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_pthread.h>
#include <esp_vfs_eventfd.h>
#endif

namespace tailscale_shim
{

namespace
{
std::mutex gLogMutex;
std::deque<std::pair<LogLevel, std::string>> gLogQueue;
/// Bound the queue: a wedged scheduler must not let workers grow it forever.
constexpr size_t TS_LOG_QUEUE_LIMIT = 64;
} // namespace

void deferLog(LogLevel level, const char *format, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gLogQueue.size() >= TS_LOG_QUEUE_LIMIT)
        return; // drop the newest rather than grow without bound
    gLogQueue.emplace_back(level, buf);
}

void drainDeferredLogs()
{
    std::deque<std::pair<LogLevel, std::string>> batch;
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        batch.swap(gLogQueue);
    }

    for (const auto &entry : batch) {
        switch (entry.first) {
        case LogLevel::Info:
            LOG_INFO("%s", entry.second.c_str());
            break;
        case LogLevel::Warn:
            LOG_WARN("%s", entry.second.c_str());
            break;
        case LogLevel::Error:
            LOG_ERROR("%s", entry.second.c_str());
            break;
        }
    }
}

/// Chunk size for a single `ts_tcp_recv`. One MTU-ish read keeps the reader
/// responsive without making the buffer churn.
static constexpr size_t TS_READ_CHUNK = 1024;

// ---------------------------------------------------------------------------
// ConnState
// ---------------------------------------------------------------------------

ConnState::~ConnState()
{
    // Reached only once both workers have dropped their references and every
    // client handle is gone, so nothing can still hold the `&tcp_stream` that
    // would make this close an aliasing violation. See the struct comment.
    if (stream) {
        ts_tcp_close(stream);
        stream = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Connection workers
//
// Both take the shared state by value: that reference is what keeps the state
// alive while they are detached, and dropping it on return is what eventually
// triggers `~ConnState` and closes the socket.
// ---------------------------------------------------------------------------

static void readerLoop(std::shared_ptr<ConnState> st)
{
    std::vector<uint8_t> chunk(TS_READ_CHUNK);

    while (st->running.load()) {
        // Backpressure: park rather than let a stalled consumer grow the
        // buffer without bound. Not reading is what pushes back on the peer.
        {
            std::unique_lock<std::mutex> lock(st->rxMutex);
            st->rxDrained.wait(lock, [&] { return !st->running.load() || st->rxBuf.size() < TS_RX_BUFFER_LIMIT; });
            if (!st->running.load())
                break;
        }

        int n = ts_tcp_recv(st->stream, chunk.data(), chunk.size());
        if (n == 0) {
            TS_LOG_INFO("Tailscale: peer closed connection");
            break;
        }
        if (n < 0) {
            TS_LOG_WARN("Tailscale: recv failed (%d), dropping connection", n);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(st->rxMutex);
            st->rxBuf.insert(st->rxBuf.end(), chunk.begin(), chunk.begin() + n);
        }
    }

    st->peerUp.store(false);
    // Release the writer, which may be parked waiting for work that will now
    // never come.
    st->txPending.notify_all();
    st->txDrained.notify_all();
}

static void writerLoop(std::shared_ptr<ConnState> st)
{
    std::vector<uint8_t> chunk;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(st->txMutex);
            st->txPending.wait(lock,
                               [&] { return !st->running.load() || !st->peerUp.load() || !st->txBuf.empty(); });

            if (st->txBuf.empty()) {
                // Nothing left to flush and nobody expects more.
                if (!st->running.load() || !st->peerUp.load())
                    break;
                continue;
            }

            chunk.assign(st->txBuf.begin(), st->txBuf.end());
            st->txBuf.clear();
        }
        // Signal *handoff*, not delivery, and do it before sending. StreamAPI
        // calls flush() on the scheduler thread after every packet; waiting
        // for the send to complete would park that thread on a stalled DERP
        // relay for an unbounded time -- the exact stall this whole design
        // exists to avoid.
        st->txDrained.notify_all();

        // `ts_tcp_send` returns once *at least one* byte is written, so a
        // single call is not enough -- loop until the chunk is gone.
        size_t off = 0;
        while (off < chunk.size()) {
            int n = ts_tcp_send(st->stream, chunk.data() + off, chunk.size() - off);
            if (n <= 0) {
                TS_LOG_WARN("Tailscale: send failed (%d), dropping connection", n);
                st->peerUp.store(false);
                break;
            }
            off += static_cast<size_t>(n);
        }

        if (!st->peerUp.load())
            break;
    }

    st->txDrained.notify_all();
}

/**
 * Give threads created here a usable stack before spawning them.
 *
 * std::thread on ESP-IDF is a pthread, and pthreads get
 * CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT, which is 2048 bytes in the Arduino
 * libs. These threads call into the Rust tailscale stack, so 2 KiB is not
 * close to enough -- and a FreeRTOS stack overflow aborts the node rather than
 * failing the call, which makes it look like a crash in whatever ran next.
 *
 * esp_pthread_set_cfg applies to the calling thread and is inherited by
 * threads it creates, so this must run on the thread doing the spawning,
 * immediately before it.
 */
// Stack sizes, in bytes.
//
// TS_TOKIO_STACK only selects placement: Rust sets the pthread attr size
// explicitly (thread_stack_size in ts_ffi, 64 KiB), which overrides this.
//
// The bring-up thread must be internal -- it does the key-state file I/O -- and
// it also polls the join future via block_on, so it gets the largest internal
// stack. Affordable because tokio's stacks are no longer in internal DRAM.
// Everything else only touches sockets, so it lives in PSRAM with room to
// spare. None of these are high-water-mark measurements yet.
static constexpr size_t TS_TOKIO_STACK = 64 * 1024;

// FreeRTOS priority for every Tailscale thread, and everything tokio spawns.
//
// Equal to Arduino's loopTask (created at priority 1 in cores/esp32/main.cpp),
// which runs all of Meshtastic. pthreads default to 5
// (CONFIG_PTHREAD_TASK_PRIO_DEFAULT), and under FreeRTOS a busy higher-priority
// task starves lower ones outright -- observed when a real tailnet join made
// tailscale-rs probe every DERP region at once: ts_tokio held CPU 1 for ~94s,
// loopTask never ran, and the 90s task watchdog reset the chip. At equal
// priority FreeRTOS time-slices each 1 ms tick, so Tailscale work can slow the
// mesh down but never stop it.
#ifdef ARCH_ESP32
static constexpr size_t TS_THREAD_PRIORITY = 1; // esp_pthread_cfg_t::prio is a size_t
#endif
static constexpr size_t TS_BRINGUP_STACK = 48 * 1024;
static constexpr size_t TS_IO_STACK = 24 * 1024;

#ifdef MESHTASTIC_SIMULATOR_NO_LORA
#include <lwip/sockets.h>
/// SIMULATOR DIAGNOSTIC: how many lwIP sockets are free right now, found by
/// opening UDP sockets until the table is full and closing them again. The
/// Arduino libs cap lwIP at 16 sockets (CONFIG_LWIP_MAX_SOCKETS), shared with
/// Meshtastic's API server, web servers, mDNS and NTP -- and a real tailnet
/// join exhausted them. Briefly holds every free socket, so anything else that
/// opens one in that instant fails: fine for a diagnostic build, not for a node.
static int countFreeSockets()
{
    int fds[32];
    int n = 0;
    while (n < 32) {
        int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            break;
        fds[n++] = fd;
    }
    for (int i = 0; i < n; i++)
        lwip_close(fds[i]);
    return n;
}
#endif

/// Report internal-DRAM headroom. Thread stacks come from here, and how much is
/// left depends on what else has started (WiFi, mDNS, two web servers), so the
/// sizes above are only defensible next to a measurement.
static void logInternalHeap(const char *when)
{
#ifdef ARCH_ESP32
    // esp_rom_printf, not TS_LOG_INFO. The deferred queue is drained by the
    // scheduler, so anything logged through it is lost when the point of
    // logging is immediately followed by an abort -- which is exactly the case
    // this measurement exists to diagnose. esp_rom_printf goes straight to the
    // UART and is safe from any thread.
    esp_rom_printf("Tailscale: heap %s: internal %u free (%u largest), psram %u free\n", when,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    (void)when;
#endif
}

/**
 * Where a thread's stack is allocated from.
 *
 * Internal DRAM is scarce -- by the time the tailnet comes up, WiFi, mDNS and
 * two web servers have taken most of the ~220KB the chip starts with, and
 * pthread_create fails with ENOMEM (tokio reports "OS can't spawn worker
 * thread: Not enough space" and panics, which aborts the node).
 *
 * PSRAM has ~2MB free and the board sets CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY,
 * but a stack there carries a hard restriction: a task running on an external
 * stack must never be the one that disables the flash cache. Any SPI flash
 * write does that, so a thread touching LittleFS trips
 *
 *   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
 *                  (esp_task_stack_is_sane_cache_disabled())
 *
 * So: threads that do file I/O get Internal, threads that only do network and
 * compute get Psram.
 */
enum class StackMem { Internal, Psram };

/**
 * Configure the stack for threads created *after* this call.
 *
 * esp_pthread_set_cfg applies to the calling thread and is inherited by threads
 * it creates -- it does not move the caller's own stack, which is already
 * allocated. That is what lets the bring-up thread keep an internal stack for
 * its LittleFS access while handing PSRAM stacks to the tokio workers it
 * spawns moments later.
 */
static void setThreadStack(size_t bytes, const char *name, StackMem where)
{
#ifdef ARCH_ESP32
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = bytes;
    cfg.thread_name = name;
    cfg.prio = TS_THREAD_PRIORITY;
    // MALLOC_CAP_8BIT is required either way: a task stack must be byte-accessible.
    cfg.stack_alloc_caps =
        (where == StackMem::Psram) ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    cfg.inherit_cfg = true;
    esp_pthread_set_cfg(&cfg);
#else
    (void)bytes;
    (void)name;
    (void)where;
#endif
}

/// Undo setThreadStack for the calling thread, for threads that spawn ours but
/// are not ours (Meshtastic's main loop).
static void restoreThreadStackDefaults()
{
#ifdef ARCH_ESP32
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&cfg);
#endif
}

// ---------------------------------------------------------------------------
// TailscaleConn
// ---------------------------------------------------------------------------

TailscaleConn::TailscaleConn(ts_tcp_stream *stream) : st(std::make_shared<ConnState>(stream)) {}

std::shared_ptr<TailscaleConn> TailscaleConn::create(ts_tcp_stream *stream)
{
    auto conn = std::shared_ptr<TailscaleConn>(new TailscaleConn(stream));

    // Detached on purpose: neither worker can be interrupted while parked in a
    // blocking ts_ffi call, so joining them would be a hang waiting to happen.
    // Their `shared_ptr<ConnState>` copies keep the state valid regardless.
    setThreadStack(TS_IO_STACK, "ts_io", StackMem::Psram);
    std::thread(readerLoop, conn->st).detach();
    std::thread(writerLoop, conn->st).detach();

    return conn;
}

TailscaleConn::~TailscaleConn()
{
    close();
}

size_t TailscaleConn::available()
{
    std::lock_guard<std::mutex> lock(st->rxMutex);
    return st->rxBuf.size();
}

int TailscaleConn::peek()
{
    std::lock_guard<std::mutex> lock(st->rxMutex);
    if (st->rxBuf.empty())
        return -1;
    return st->rxBuf.front();
}

int TailscaleConn::read()
{
    int b;
    {
        std::lock_guard<std::mutex> lock(st->rxMutex);
        if (st->rxBuf.empty())
            return -1;
        b = st->rxBuf.front();
        st->rxBuf.pop_front();
    }
    st->rxDrained.notify_one();
    return b;
}

size_t TailscaleConn::read(uint8_t *buf, size_t size)
{
    size_t n;
    {
        std::lock_guard<std::mutex> lock(st->rxMutex);
        n = std::min(size, st->rxBuf.size());
        std::copy(st->rxBuf.begin(), st->rxBuf.begin() + n, buf);
        st->rxBuf.erase(st->rxBuf.begin(), st->rxBuf.begin() + n);
    }
    if (n)
        st->rxDrained.notify_one();
    return n;
}

size_t TailscaleConn::write(const uint8_t *buf, size_t size)
{
    size_t n;
    {
        std::lock_guard<std::mutex> lock(st->txMutex);
        if (!st->peerUp.load() || !st->running.load())
            return 0;

        // All or nothing. StreamAPI::emitTxBuffer hands us a whole framed
        // packet and ignores our return value, so accepting a prefix would put
        // a truncated frame on the wire and the peer would fail to decode --
        // the same torn-frame class of bug the `streamLock` in StreamAPI.h was
        // added to fix. Dropping the connection is recoverable; a corrupt
        // stream is not.
        const size_t room = st->txBuf.size() < TS_TX_BUFFER_LIMIT ? TS_TX_BUFFER_LIMIT - st->txBuf.size() : 0;
        if (size > room) {
            TS_LOG_ERROR("Tailscale: tx queue full (%u queued, %u needed), dropping connection",
                      (unsigned)st->txBuf.size(), (unsigned)size);
            st->peerUp.store(false);
            return 0;
        }
        n = size;
        st->txBuf.insert(st->txBuf.end(), buf, buf + n);
    }
    st->txPending.notify_one();
    return n;
}

void TailscaleConn::flush()
{
    std::unique_lock<std::mutex> lock(st->txMutex);
    st->txDrained.wait(lock, [&] { return st->txBuf.empty() || !st->peerUp.load(); });
}

bool TailscaleConn::connected()
{
    // Stay "connected" while buffered rx data remains, so StreamAPI gets to
    // drain the last frames a peer sent before hanging up.
    if (st->peerUp.load())
        return true;
    std::lock_guard<std::mutex> lock(st->rxMutex);
    return !st->rxBuf.empty();
}

void TailscaleConn::close()
{
    if (!st)
        return;
    st->running.store(false);
    st->rxDrained.notify_all();
    st->txPending.notify_all();
    st->txDrained.notify_all();
}

std::string TailscaleConn::remoteAddr()
{
    if (!st || !st->stream)
        return {};

    ts_sockaddr addr = ts_tcp_remote_addr(st->stream);
    char buf[64];
    if (addr.sa_family == TS_AF_INET) {
        const uint8_t *a = addr.sa_data.sockaddr_in.sin_addr;
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], addr.sa_data.sockaddr_in.sin_port);
        return buf;
    }
    return "[ipv6]";
}

} // namespace tailscale_shim

// ---------------------------------------------------------------------------
// TailscaleClient
// ---------------------------------------------------------------------------

int TailscaleClient::available()
{
    return conn ? static_cast<int>(conn->available()) : 0;
}

int TailscaleClient::read()
{
    return conn ? conn->read() : -1;
}

int TailscaleClient::read(uint8_t *buf, size_t size)
{
    return conn ? static_cast<int>(conn->read(buf, size)) : -1;
}

int TailscaleClient::peek()
{
    return conn ? conn->peek() : -1;
}

size_t TailscaleClient::write(uint8_t b)
{
    return write(&b, 1);
}

size_t TailscaleClient::write(const uint8_t *buf, size_t size)
{
    return conn ? conn->write(buf, size) : 0;
}

void TailscaleClient::flush()
{
    if (conn)
        conn->flush();
}

void TailscaleClient::stop()
{
    if (conn) {
        conn->close();
        conn.reset();
    }
}

uint8_t TailscaleClient::connected()
{
    return (conn && conn->connected()) ? 1 : 0;
}

int TailscaleClient::connect(IPAddress, uint16_t)
{
    LOG_ERROR("Tailscale: outbound connect is not supported by this shim");
    return 0;
}

int TailscaleClient::connect(const char *, uint16_t)
{
    LOG_ERROR("Tailscale: outbound connect is not supported by this shim");
    return 0;
}

// ---------------------------------------------------------------------------
// TailscaleServer
// ---------------------------------------------------------------------------

TailscaleServer::AcceptState::~AcceptState()
{
    // Same ownership rule as ConnState: only safe once the acceptor has let go.
    if (listener) {
        ts_tcp_close_listener(listener);
        listener = nullptr;
    }
}

void TailscaleServer::begin()
{
    if (accepting) {
        LOG_WARN("Tailscale: server already starting/listening on port %d", port);
        return;
    }

    accepting = std::make_shared<AcceptState>();
    accepting->port = port;

    // Everything the tailnet needs -- ts_init, ts_ipv4_addr, ts_tcp_listen --
    // blocks, and this is called from setup(). Hand the lot to a thread and
    // return, or a slow control server delays the radio coming up. On ESP32
    // that delay would be a watchdog reset rather than just a late boot.
    // The bring-up thread runs the tailnet join, which goes through the TLS
    // handshake, so it needs the deepest stack of the three.
    tailscale_shim::setThreadStack(tailscale_shim::TS_BRINGUP_STACK, "ts_bringup", tailscale_shim::StackMem::Internal);
    std::thread(bringUpLoop, accepting).detach();
    // This runs on Meshtastic's main thread, and the pthread config is sticky
    // per thread: without this, any pthread it created later would silently
    // inherit a 48 KiB internal-DRAM stack.
    tailscale_shim::restoreThreadStackDefaults();
}

void TailscaleServer::bringUpLoop(std::shared_ptr<AcceptState> as)
{
    // Retry until the listener is up, rather than giving up after one failure.
    // This used to return on the first error, so a single failed step -- or a
    // control reconnect misread as a rejection -- left the node off the tailnet
    // until reboot. Backoff 5s doubling to 5 minutes: generous enough not to
    // spend a battery node's charge on a network that is down, short enough to
    // come back without anyone noticing.
    for (uint32_t attempt = 1; as->running.load(); attempt++) {
        if (TailscaleTransport::begin()) {
            ts_device *dev = TailscaleTransport::device();

            ts_sockaddr addr = {};
            addr.sa_family = TS_AF_INET;
            addr.sa_data.sockaddr_in.sin_port = static_cast<uint16_t>(as->port);
            if (ts_ipv4_addr(dev, &addr.sa_data.sockaddr_in.sin_addr) == 0) {
                ts_tcp_listener *listener = ts_tcp_listen(dev, &addr);
                if (listener) {
                    as->listener = listener;
                    break;
                }
                TS_LOG_ERROR("Tailscale: listen on port %d failed", as->port);
            } else {
                TS_LOG_ERROR("Tailscale: no tailnet IPv4 assigned, not listening");
            }
            TailscaleTransport::end();
        }

        const uint32_t delay = std::min<uint32_t>(5u << std::min<uint32_t>(attempt - 1, 6), 300);
        TS_LOG_WARN("Tailscale: bring-up attempt %u failed, retrying in %us", (unsigned)attempt, (unsigned)delay);
        for (uint32_t i = 0; i < delay && as->running.load(); i++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!as->listener)
        return; // shutting down

    TS_LOG_INFO("Tailscale: API server listening on %s:%d", TailscaleTransport::ipv4().c_str(), as->port);

    while (as->running.load()) {
        ts_tcp_stream *stream = ts_tcp_accept(as->listener);
        if (!stream) {
            TS_LOG_WARN("Tailscale: accept failed, acceptor exiting");
            break;
        }
        if (!as->running.load()) {
            ts_tcp_close(stream);
            break;
        }

        auto conn = tailscale_shim::TailscaleConn::create(stream);
        TS_LOG_INFO("Tailscale: incoming connection from %s", conn->remoteAddr().c_str());

        std::lock_guard<std::mutex> lock(as->mutex);
        as->pending.push_back(std::move(conn));
    }
}

TailscaleClient TailscaleServer::available()
{
    if (!accepting)
        return TailscaleClient();

    std::lock_guard<std::mutex> lock(accepting->mutex);
    if (accepting->pending.empty())
        return TailscaleClient();

    auto conn = accepting->pending.front();
    accepting->pending.pop_front();
    return TailscaleClient(std::move(conn));
}

TailscaleServer::~TailscaleServer()
{
    if (accepting)
        accepting->running.store(false);
    // The acceptor is parked in `ts_tcp_accept` and cannot be woken with
    // unmodified ts_ffi; it holds the state alive and tears the listener down
    // on its way out. See TailscaleConn::close() for the upstream fix.
}

// ---------------------------------------------------------------------------
// TailscaleTransport
// ---------------------------------------------------------------------------

namespace TailscaleTransport
{

static ts_device *gDevice = nullptr;

static std::string gStateFile;
static std::string gAuthKey;
static std::string gHostname;

void configure(const std::string &stateFile, const std::string &authKey, const std::string &hostname)
{
    gStateFile = stateFile;
    gAuthKey = authKey;
    gHostname = hostname;
}

/**
 * Register the eventfd VFS, which tokio's IO driver needs to exist.
 *
 * tokio builds a waker out of an eventfd. On Linux that syscall is always
 * there; on ESP-IDF eventfd is a VFS driver that nothing registers for you, so
 * eventfd() fails with EACCES and *runtime construction itself* fails. What
 * surfaced was a Rust panic inside LazyLock<Runtime> with
 * `Os { code: 13, kind: PermissionDenied }` -- which names neither eventfd nor
 * tokio, and aborts the node into a reboot loop because a panic on a detached
 * thread takes the whole process down.
 *
 * Registering is idempotent from our side: a second call returns
 * ESP_ERR_INVALID_STATE, which is not an error for us. max_fds is the default
 * 5; tokio needs one.
 */
static void registerEventFd()
{
#ifdef ARCH_ESP32
    static std::once_flag once;
    std::call_once(once, [] {
        esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
        esp_err_t err = esp_vfs_eventfd_register(&cfg);
        if (err == ESP_OK)
            TS_LOG_INFO("Tailscale: registered eventfd VFS for the tokio runtime");
        else if (err == ESP_ERR_INVALID_STATE)
            TS_LOG_INFO("Tailscale: eventfd VFS already registered");
        else
            TS_LOG_ERROR("Tailscale: eventfd VFS registration failed (%d) -- the tokio "
                         "runtime will fail to start",
                         (int)err);
    });
#endif
}

bool begin()
{
    if (gDevice)
        return true;

    // Must precede every ts_ffi call: the first one to touch TOKIO_RUNTIME
    // forces the LazyLock, and that is where the eventfd is needed.
    registerEventFd();

    // PSRAM for every thread tokio creates from here on. Safe only because no
    // tokio thread does file I/O on ESP-IDF: tailscale-rs's key-state access
    // runs on the thread polling block_on -- this one, whose stack is internal
    // -- rather than on tokio's blocking pool (key_fs in src/config.rs). Before
    // that change it went through tokio::fs, and a PSRAM-stacked pool thread
    // disabling the flash cache asserted in esp_task_stack_is_sane_cache_disabled.
    //
    // inherit_cfg carries this to threads tokio's own threads spawn later
    // (the blocking pool, for DNS), which also never touch flash.
    tailscale_shim::logInternalHeap("before tokio runtime");
#ifdef MESHTASTIC_SIMULATOR_NO_LORA
    esp_rom_printf("Tailscale: lwIP sockets free before tailnet start: %d\n", tailscale_shim::countFreeSockets());
#endif
    tailscale_shim::setThreadStack(tailscale_shim::TS_TOKIO_STACK, "ts_tokio", tailscale_shim::StackMem::Psram);

    ts_init_tracing();

    // Node identity persists across restarts so the node keeps its tailnet IP;
    // `true` regenerates the file if it is missing or corrupt, which is what
    // makes first boot work without a separate provisioning step.
    ts_persisted_key_state keys = {};
    if (ts_load_key_file(gStateFile.c_str(), true, &keys) < 0) {
        TS_LOG_ERROR("Tailscale: could not load or create key state at %s", gStateFile.c_str());
        return false;
    }
    tailscale_shim::logInternalHeap("after runtime + key state");

    // Zero-initialised for defaults -- the header is explicit that leaving
    // this uninitialised is UB.
    ts_config config = {};
    config.hostname = gHostname.c_str();
    config.key_state = &keys;

    TS_LOG_INFO("Tailscale: joining tailnet as %s, this can take a few seconds", gHostname.c_str());

    gDevice = ts_init(&config, gAuthKey.c_str());
    tailscale_shim::logInternalHeap("after join attempt");
    if (!gDevice) {
        TS_LOG_ERROR("Tailscale: failed to join tailnet");
        return false;
    }

    // A non-null device is NOT proof of a join. When the control server
    // rejects the node (a bad or spent auth key -> MachineNotAuthorized),
    // ts_init still hands back a device and only the absence of an address
    // gives it away. Without this check the log claimed "joined tailnet" with
    // an empty IP and the real cause was buried in Rust-side tracing.
    const std::string addr = ipv4();
    if (addr.empty()) {
        // Reached only on a non-transient error now: ts_ipv4_addr waits through
        // control reconnects. This used to say the control server had rejected
        // the node, which was also what a reconnect in progress looked like --
        // and tearing down there killed the reconnection. The cause, bad key
        // included, is in the tailscale-rs log line just before this one.
        TS_LOG_ERROR("Tailscale: could not obtain a tailnet address (cause in the tailscale-rs log above)");
        end();
        return false;
    }

    TS_LOG_INFO("Tailscale: joined tailnet as %s (%s)", gHostname.c_str(), addr.c_str());
    return true;
}

void end()
{
    if (gDevice) {
        ts_deinit(gDevice);
        gDevice = nullptr;
    }
}

ts_device *device()
{
    return gDevice;
}

std::string ipv4()
{
    if (!gDevice)
        return {};

    ts_in_addr_t a = {};
    if (ts_ipv4_addr(gDevice, &a) != 0)
        return {};

    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    return buf;
}

} // namespace TailscaleTransport

#endif // MESHTASTIC_TAILSCALE
