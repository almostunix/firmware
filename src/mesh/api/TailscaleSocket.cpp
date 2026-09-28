#include "TailscaleSocket.h"

#ifdef MESHTASTIC_TAILSCALE

#include "Throttle.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef ARCH_ESP32
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_rom_sys.h>
#include <esp_pthread.h>
#include <esp_vfs_eventfd.h>
#endif

namespace tailscale_shim
{

namespace
{
std::mutex gLogMutex;
// A vector, not a deque: libstdc++'s empty deque allocates ~544 bytes, and this is drained every 100 ms.
std::vector<std::pair<LogLevel, std::string>> gLogQueue;
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
    std::vector<std::pair<LogLevel, std::string>> batch;
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        if (gLogQueue.empty())
            return;
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

StreamOps streamOps = {ts_tcp_recv, ts_tcp_send, ts_tcp_shutdown, ts_tcp_close};

/// Chunk size for a single `ts_tcp_recv`. One MTU-ish read keeps the reader
/// responsive without making the buffer churn.
static constexpr size_t TS_READ_CHUNK = 1024;

ConnState::~ConnState()
{
    // Only runs once both workers and every handle are gone, so nothing still holds the stream.
    if (stream) {
        streamOps.close(stream);
        stream = nullptr;
    }
}

// Workers hold the state by value; the last one out triggers ~ConnState and the close.

static void readerLoop(std::shared_ptr<ConnState> st)
{
    ByteVector chunk(TS_READ_CHUNK);

    while (st->running.load()) {
        // Backpressure: park rather than let a stalled consumer grow the
        // buffer without bound. Not reading is what pushes back on the peer.
        {
            std::unique_lock<std::mutex> lock(st->rxMutex);
            st->rxDrained.wait(lock, [&] { return !st->running.load() || st->rxBuf.size() < TS_RX_BUFFER_LIMIT; });
            if (!st->running.load())
                break;
        }

        int n = streamOps.recv(st->stream, chunk.data(), chunk.size());
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
    ByteVector chunk;

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

            // Swap rather than copy; the producer refills the emptied buffer, keeping its capacity.
            chunk.clear();
            chunk.swap(st->txBuf);
        }
        // Handoff, not delivery: StreamAPI flushes on the scheduler after every packet.
        st->txDrained.notify_all();

        // `ts_tcp_send` returns once *at least one* byte is written, so a
        // single call is not enough -- loop until the chunk is gone.
        size_t off = 0;
        while (off < chunk.size()) {
            int n = streamOps.send(st->stream, chunk.data() + off, chunk.size() - off);
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

// Stack sizes in bytes. TS_TOKIO_STACK only sets placement (Rust sets the size); bring-up is internal for file I/O.
static constexpr size_t TS_TOKIO_STACK = 64 * 1024;

// Loop-task priority: at pthreads' default of 5 a busy tokio worker starved Meshtastic into the 90 s watchdog.
#ifdef ARCH_ESP32
static constexpr size_t TS_THREAD_PRIORITY = 1; // esp_pthread_cfg_t::prio is a size_t
#endif
static constexpr size_t TS_BRINGUP_STACK = 32 * 1024; // ~2x measured; freed once the acceptor takes over
static constexpr size_t TS_ACCEPT_STACK = 16 * 1024;  // PSRAM: accept + spawning connection threads, no flash I/O
static constexpr size_t TS_IO_STACK = 24 * 1024;

#ifdef MESHTASTIC_SIMULATOR_NO_LORA
#include <lwip/sockets.h>
/// Simulator diagnostic: free lwIP sockets, by opening UDP sockets until the table is full.
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

/// Internal-RAM headroom, with MESHTASTIC_TAILSCALE_DIAGNOSTICS. esp_rom_printf, so it survives an abort.
static void logInternalHeap(const char *when)
{
#if defined(ARCH_ESP32) && defined(MESHTASTIC_TAILSCALE_DIAGNOSTICS)
    esp_rom_printf("Tailscale: heap %s: internal %u free (%u largest), psram %u free\n", when,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    (void)when;
#endif
}

/// Stacks for file I/O must be internal: a task on a PSRAM stack may not disable the flash cache.
enum class StackMem { Internal, Psram };

/// Stack for threads the caller creates after this; esp_pthread_set_cfg is per thread and inherited.
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

TailscaleConn::TailscaleConn(ts_tcp_stream *stream) : st(std::make_shared<ConnState>(stream)) {}

std::shared_ptr<TailscaleConn> TailscaleConn::create(ts_tcp_stream *stream)
{
    auto conn = std::shared_ptr<TailscaleConn>(new TailscaleConn(stream));

    // Detached: a blocked ts_ffi call can't be interrupted, and the shared state outlives the handles.
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

        // All or nothing: StreamAPI ignores the return, so a prefix would put a torn frame on the wire.
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
    const uint32_t start = millis();
    // Bounded: a writer stuck in ts_tcp_send behind a vanished peer used to hold the mesh loop here indefinitely.
    while (!st->txBuf.empty() && st->peerUp.load()) {
        if (!Throttle::isWithinTimespanMs(start, TS_FLUSH_TIMEOUT_MS)) {
            TS_LOG_WARN("Tailscale: peer took no data for %us, dropping connection", (unsigned)(TS_FLUSH_TIMEOUT_MS / 1000));
            st->peerUp.store(false);
            st->txPending.notify_all();
            break;
        }
        st->txDrained.wait_for(lock, std::chrono::milliseconds(100));
    }
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
    // FIN, so a client the node drops (idle timeout, replaced by a newer one) knows at once rather than at its next write.
    if (st->running.exchange(false) && st->stream)
        streamOps.shutdown(st->stream);
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

    // Joining blocks for seconds, and this runs from setup(), so bring-up gets its own thread.
    tailscale_shim::setThreadStack(tailscale_shim::TS_BRINGUP_STACK, "ts_bringup", tailscale_shim::StackMem::Internal);
    std::thread(bringUpLoop, accepting).detach();
    // esp_pthread_set_cfg is sticky per thread: restore it for the scheduler's own later threads.
    tailscale_shim::restoreThreadStackDefaults();
}

void TailscaleServer::bringUpLoop(std::shared_ptr<AcceptState> as)
{
    // Retry until listening; backoff 5 s doubling to 5 min spares a battery when the network is down.
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

#if defined(ARCH_ESP32) && defined(MESHTASTIC_TAILSCALE_DIAGNOSTICS)
    // The join ran on this thread, so this is TS_BRINGUP_STACK's measured peak.
    esp_rom_printf("Tailscale: bring-up thread stack: %u of %u bytes never used\n",
                   (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)tailscale_shim::TS_BRINGUP_STACK);
#endif

    TS_LOG_INFO("Tailscale: API server listening on %s:%d", TailscaleTransport::ipv4().c_str(), as->port);

    // Only bring-up touches flash, so the long-lived acceptor runs on a PSRAM stack and this thread's
    // internal-RAM stack is freed when it returns.
    tailscale_shim::setThreadStack(tailscale_shim::TS_ACCEPT_STACK, "ts_accept", tailscale_shim::StackMem::Psram);
    std::thread(acceptLoop, as).detach();
}

void TailscaleServer::acceptLoop(std::shared_ptr<AcceptState> as)
{
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
#if defined(ARCH_ESP32) && defined(MESHTASTIC_TAILSCALE_DIAGNOSTICS)
        static bool reportedAcceptStack = false;
        if (!reportedAcceptStack) {
            reportedAcceptStack = true;
            esp_rom_printf("Tailscale: accept thread stack after first accept: %u of %u bytes never used\n",
                           (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)tailscale_shim::TS_ACCEPT_STACK);
            tailscale_shim::logInternalHeap("at first accept");
        }
#endif

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
    // The acceptor may be blocked in ts_tcp_accept; it holds the state and closes the listener on exit.
}

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

/// tokio's IO driver needs an eventfd, which ESP-IDF only provides once this VFS is registered.
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

    // PSRAM stacks for tokio's threads: safe because key-state file I/O stays on this internal-stack thread.
    tailscale_shim::logInternalHeap("before tokio runtime");
#ifdef MESHTASTIC_SIMULATOR_NO_LORA
    esp_rom_printf("Tailscale: lwIP sockets free before tailnet start: %d\n", tailscale_shim::countFreeSockets());
#endif
    tailscale_shim::setThreadStack(tailscale_shim::TS_TOKIO_STACK, "ts_tokio", tailscale_shim::StackMem::Psram);

    ts_init_tracing();

    // `true` creates the identity file on first boot; it keeps the node's tailnet IP across restarts.
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

    // ts_init returns a device even when control rejects the node; only a missing address shows it.
    const std::string addr = ipv4();
    if (addr.empty()) {
        // Non-transient: ts_ipv4_addr waits through control reconnects. The cause is in the log above.
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
