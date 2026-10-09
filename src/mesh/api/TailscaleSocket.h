#pragma once

#include "configuration.h"

#ifdef MESHTASTIC_TAILSCALE

#include <Arduino.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef ARCH_ESP32
#include <esp_heap_caps.h>
#endif

#include "tailscale.h"

// Arduino Client/Server over tailscale-rs's blocking TCP (ts_ffi), so ServerAPI can serve a tailnet.
// Design notes: tailscale/README.md, "How the socket shim works".

namespace tailscale_shim
{

// LOG_ is not thread-safe, so worker threads queue lines for the scheduler to emit.
enum class LogLevel { Info, Warn, Error };

/// Queue a log line. Safe from any thread; never blocks on the scheduler.
void deferLog(LogLevel level, const char *format, ...) __attribute__((format(printf, 2, 3)));

/// Emit everything queued. Scheduler thread only.
void drainDeferredLogs();

} // namespace tailscale_shim

#define TS_LOG_INFO(...) tailscale_shim::deferLog(tailscale_shim::LogLevel::Info, __VA_ARGS__)
#define TS_LOG_WARN(...) tailscale_shim::deferLog(tailscale_shim::LogLevel::Warn, __VA_ARGS__)
#define TS_LOG_ERROR(...) tailscale_shim::deferLog(tailscale_shim::LogLevel::Error, __VA_ARGS__)

namespace tailscale_shim
{

#ifdef ARCH_ESP32
/// Connection buffers in PSRAM: malloc puts anything under 4 KB in internal RAM, which WiFi needs.
template <class T> struct PsramAllocator {
    using value_type = T;
    PsramAllocator() = default;
    template <class U> PsramAllocator(const PsramAllocator<U> &) {}

    T *allocate(size_t n)
    {
        if (n > SIZE_MAX / sizeof(T))
            abort(); // what std::allocator does without exceptions
        void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p)
            p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
        if (!p)
            abort();
        return static_cast<T *>(p);
    }
    void deallocate(T *p, size_t) { heap_caps_free(p); }

    template <class U> bool operator==(const PsramAllocator<U> &) const { return true; }
    template <class U> bool operator!=(const PsramAllocator<U> &) const { return false; }
};
using ByteDeque = std::deque<uint8_t, PsramAllocator<uint8_t>>;
using ByteVector = std::vector<uint8_t, PsramAllocator<uint8_t>>;
#else
using ByteDeque = std::deque<uint8_t>;
using ByteVector = std::vector<uint8_t>;
#endif

/// The ts_ffi calls a connection makes. A seam for unit tests, which substitute a fake peer.
struct StreamOps {
    decltype(&ts_tcp_recv) recv;
    decltype(&ts_tcp_send) send;
    decltype(&ts_tcp_shutdown) shutdown;
    decltype(&ts_tcp_close) close;
};
extern StreamOps streamOps;

/// The reader stops here until the scheduler drains it: TCP backpressure.
static constexpr size_t TS_RX_BUFFER_LIMIT = 8 * 1024;

/// write() never blocks; a frame that doesn't fit drops the connection instead.
static constexpr size_t TS_TX_BUFFER_LIMIT = 16 * 1024;

/// Longest flush() waits for the writer before treating the peer as gone, as a stalled lwIP write is.
static constexpr uint32_t TS_FLUSH_TIMEOUT_MS = 5000;

/// Shared by a connection's handles and detached workers; ~ConnState closes the socket, once none remain.
struct ConnState {
    explicit ConnState(ts_tcp_stream *s) : stream(s) {}
    ~ConnState();

    ConnState(const ConnState &) = delete;
    ConnState &operator=(const ConnState &) = delete;

    ts_tcp_stream *stream;

    std::mutex rxMutex;
    std::condition_variable rxDrained;
    ByteDeque rxBuf;

    std::mutex txMutex;
    std::condition_variable txPending;
    std::condition_variable txDrained;
    /// Swapped out whole by the writer, so queued bytes are never copied.
    ByteVector txBuf;

    /// Cleared when the peer hangs up or the link errors.
    std::atomic<bool> peerUp{true};
    /// Cleared to ask the workers to wind down.
    std::atomic<bool> running{true};
};

/// One tailnet TCP connection. Shared by shared_ptr, as ServerAPI copies its client by value.
class TailscaleConn
{
  public:
    /// Adopt an accepted stream and start its workers.
    static std::shared_ptr<TailscaleConn> create(ts_tcp_stream *stream);
    ~TailscaleConn();

    TailscaleConn(const TailscaleConn &) = delete;
    TailscaleConn &operator=(const TailscaleConn &) = delete;

    /// Buffered bytes ready to read. Never blocks.
    size_t available();
    /// Next buffered byte without consuming it, or -1. Never blocks.
    int peek();
    /// Consume one buffered byte, or -1 if none. Never blocks.
    int read();
    /// Consume up to `size` buffered bytes. Never blocks. Returns count moved.
    size_t read(uint8_t *buf, size_t size);

    /// Queue bytes for the writer, all or nothing (0 drops the connection). Never blocks.
    size_t write(const uint8_t *buf, size_t size);
    /// Wait, at most TS_FLUSH_TIMEOUT_MS, for the writer to take the queue: handoff, not delivery.
    void flush();

    /// True while the peer is up or buffered rx data remains unread.
    bool connected();

    /// Stop the workers and send the peer a FIN. Any thread; never blocks.
    void close();

    /// Remote endpoint, for logging.
    std::string remoteAddr();

  private:
    explicit TailscaleConn(ts_tcp_stream *stream);

    std::shared_ptr<ConnState> st;
};

} // namespace tailscale_shim

/// Client over a tailnet stream; copies share a connection, and a default one means "none waiting".
class TailscaleClient : public Client
{
  public:
    TailscaleClient() = default;
    explicit TailscaleClient(std::shared_ptr<tailscale_shim::TailscaleConn> c) : conn(std::move(c)) {}

    // -- Stream / Print ----------------------------------------------------
    int available() override;
    int read() override;
    int read(uint8_t *buf, size_t size) override;
    int peek() override;
    size_t write(uint8_t b) override;
    size_t write(const uint8_t *buf, size_t size) override;
    void flush() override;

    // -- Client ------------------------------------------------------------
    void stop() override;
    uint8_t connected() override;
    operator bool() override { return conn != nullptr; }

    // The node only listens; Client makes these pure virtual.
    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;

  private:
    std::shared_ptr<tailscale_shim::TailscaleConn> conn;
};

/// Server over a tailnet listener, for APIServerPort.
class TailscaleServer : public Server
{
  public:
    explicit TailscaleServer(int port) : port(port) {}
    ~TailscaleServer();

    /// Join and listen on a background thread; available() reports nothing until then.
    void begin() override;

    /// A pending connection, or a falsy client. Never blocks.
    TailscaleClient available();

    /// APIServerPort calls accept() on Arduino-ESP32 3.x and available() elsewhere.
    TailscaleClient accept() { return available(); }

    /// `Server` inherits `Print`; a listener has nothing to write to.
    size_t write(uint8_t) override { return 0; }

  private:
    /// Shared with the detached accept thread, which may be blocked in ts_tcp_accept.
    struct AcceptState {
        ~AcceptState();
        int port = 0;
        ts_tcp_listener *listener = nullptr;
        std::atomic<bool> running{true};
        std::mutex mutex;
        std::deque<std::shared_ptr<tailscale_shim::TailscaleConn>> pending;
    };

    /// Join and listen (retrying), then hand over to acceptLoop. Both block, so neither runs on the scheduler.
    static void bringUpLoop(std::shared_ptr<AcceptState> as);
    static void acceptLoop(std::shared_ptr<AcceptState> as);

    int port;
    std::shared_ptr<AcceptState> accepting;
};

/// The process-wide tailscale-rs device. Settings come from /prefs/tailscale.conf, not ModuleConfig.
namespace TailscaleTransport
{
/// Record the settings `begin()` will use. Called on the startup thread.
void configure(const std::string &stateFile, const std::string &authKey, const std::string &hostname);
/// Join the tailnet. Blocks; idempotent; true once the node holds a tailnet IPv4.
bool begin();
/// Tear down the device, if any, so `begin()` can start over.
void end();
/// The device, or nullptr if `begin()` has not succeeded.
ts_device *device();
/// This node's tailnet IPv4 as a dotted quad, or empty if not joined.
std::string ipv4();
} // namespace TailscaleTransport

#endif // MESHTASTIC_TAILSCALE
