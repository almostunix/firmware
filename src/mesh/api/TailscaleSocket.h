#pragma once

#include "configuration.h"

#ifdef MESHTASTIC_TAILSCALE

#include <Arduino.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "tailscale.h"

/**
 * Arduino-shaped shim over the tailscale-rs C FFI (ts_ffi / libtailscalers).
 *
 * ## Why this layer exists
 *
 * Meshtastic's phone API is already generic over its transport:
 * `ServerAPI<T>` and `APIServerPort<T, U>` (mesh/api/ServerAPI.cpp) only ever
 * touch `begin()`, `available()`, `connected()`, `stop()` and the `Stream`
 * read/write surface. Exposing the API on a tailnet therefore needs no change
 * to PhoneAPI/StreamAPI at all -- only a `Client`/`Server` pair backed by
 * tailscale-rs' userspace netstack instead of lwIP.
 *
 * ## The impedance mismatch this layer absorbs
 *
 * ts_ffi's TCP surface is *blocking only*. No timeout, no poll, no
 * non-blocking variant, no "bytes available" query:
 *
 *   - `ts_tcp_accept` blocks until a peer connects.
 *   - `ts_tcp_recv`   blocks until at least one byte arrives; returns 0 on
 *                     peer hangup, negative on error.
 *   - `ts_tcp_send`   blocks until at least one byte is written, and may
 *                     perform a *partial* write, so callers must loop.
 *   - there is no `connected()` equivalent.
 *
 * Meshtastic is the opposite shape: `APIServerPort::runOnce()` is polled from
 * the cooperative OSThread scheduler roughly every 100 ms, and `available()` /
 * `read()` must return immediately. A blocking call on that thread stalls the
 * whole firmware loop, radio servicing included.
 *
 * So each connection owns two threads that block on its behalf: a reader
 * draining `ts_tcp_recv` into a bounded rx buffer, and a writer draining a tx
 * queue into `ts_tcp_send`. The scheduler thread only ever touches those
 * buffers under a mutex. The listener gets the same treatment via an acceptor
 * thread.
 *
 * The write queue is not an optimisation -- it is required. StreamAPI has two
 * independent paths that call `stream->write()` (the packet path and the log
 * record path; see the `streamLock` comment in StreamAPI.h), so writes arrive
 * from more than one thread by design.
 */

namespace tailscale_shim
{

/// Upper bound on buffered rx bytes per connection. The reader parks once this
/// is reached and resumes as the scheduler drains it, giving us TCP
/// backpressure instead of unbounded growth behind a stalled consumer.
static constexpr size_t TS_RX_BUFFER_LIMIT = 8 * 1024;

/// Upper bound on queued tx bytes. `write()` must not block the scheduler, so
/// an over-full queue reports a short write -- the contract `Print::write`
/// already has.
static constexpr size_t TS_TX_BUFFER_LIMIT = 16 * 1024;

/**
 * State shared between a connection's worker threads and its handles.
 *
 * This is deliberately split out from `TailscaleConn` so the workers can be
 * *detached* and still be guaranteed a live place to write into: each worker
 * holds its own `shared_ptr<ConnState>`, so the state outlives any handle.
 *
 * It also decides who closes the socket. `ts_tcp_close` consumes the
 * `Box<tcp_stream>` while `ts_tcp_recv` / `ts_tcp_send` hold a `&tcp_stream`,
 * so closing while a worker is inside either call would alias a Box that is
 * being dropped -- undefined behaviour on the Rust side. Putting the close in
 * `~ConnState` means it runs only once the last worker *and* the last handle
 * are gone, so no reference can still be outstanding.
 */
struct ConnState {
    explicit ConnState(ts_tcp_stream *s) : stream(s) {}
    ~ConnState();

    ConnState(const ConnState &) = delete;
    ConnState &operator=(const ConnState &) = delete;

    ts_tcp_stream *stream;

    std::mutex rxMutex;
    std::condition_variable rxDrained;
    std::deque<uint8_t> rxBuf;

    std::mutex txMutex;
    std::condition_variable txPending;
    std::condition_variable txDrained;
    std::deque<uint8_t> txBuf;

    /// Cleared when the peer hangs up or the link errors.
    std::atomic<bool> peerUp{true};
    /// Cleared to ask the workers to wind down.
    std::atomic<bool> running{true};
};

/**
 * One live tailnet TCP connection.
 *
 * Handles are shared by `shared_ptr` because `ServerAPI<T>` stores its client
 * *by value* and copies it during construction -- the same arrangement
 * `WiFiClient` uses, where copies share one refcounted socket underneath.
 */
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

    /// Queue bytes for the writer, all or nothing. Never blocks. Returns the
    /// count queued: either `size`, or 0 if the queue could not take the whole
    /// buffer (in which case the connection is dropped rather than a partial
    /// frame emitted).
    size_t write(const uint8_t *buf, size_t size);
    /// Wait until queued bytes have been handed to the writer thread.
    ///
    /// Deliberately *not* a delivery barrier: StreamAPI calls this from the
    /// scheduler thread after every packet, so blocking until bytes reach the
    /// wire would stall the firmware loop whenever the relay is slow.
    void flush();

    /// True while the peer is up or buffered rx data remains unread.
    bool connected();

    /**
     * Ask the workers to wind down. Safe from any thread; never blocks.
     *
     * The socket is not closed here -- see `~ConnState`. Deliberately accepted
     * consequence: if the peer neither sends nor hangs up, the reader stays
     * parked in `ts_tcp_recv` and the socket is reclaimed only at process exit.
     * Upstream's own `tcp_echo.c` has the same shape. The clean fix is a small
     * addition to our ts_ffi fork -- a `ts_tcp_shutdown(&stream)` issuing
     * `Command::Close` without consuming the Box, which is what `TcpStream`'s
     * `Drop` already does -- and it is worth landing before Xtensa, where a
     * parked thread costs a real stack.
     */
    void close();

    /// Remote endpoint, for logging.
    std::string remoteAddr();

  private:
    explicit TailscaleConn(ts_tcp_stream *stream);

    std::shared_ptr<ConnState> st;
};

} // namespace tailscale_shim

/**
 * `Client` over a tailnet TCP stream. Satisfies the `T` in `ServerAPI<T>`.
 *
 * Copyable and cheap to copy; copies share one connection. A
 * default-constructed instance is the "no connection" sentinel and converts to
 * false, which is what `APIServerPort::runOnce()` tests when it asks the server
 * whether anyone is waiting.
 */
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

    /**
     * Outbound connects are out of scope.
     *
     * The node is a tailnet *server*: it listens so a phone or CLI can reach
     * the existing API. `Client` makes these pure virtual, so they are
     * implemented as explicit failures rather than left to surprise anyone.
     */
    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char *host, uint16_t port) override;

  private:
    std::shared_ptr<tailscale_shim::TailscaleConn> conn;
};

/**
 * `Server` over a tailnet TCP listener. Satisfies the `U` in
 * `APIServerPort<T, U>`.
 */
class TailscaleServer : public Server
{
  public:
    explicit TailscaleServer(int port) : port(port) {}
    ~TailscaleServer();

    /// Bind the tailnet address and start the acceptor thread.
    void begin() override;

    /// Hand over a pending connection, or a falsy client if none is waiting.
    /// Never blocks -- this runs on the scheduler thread.
    TailscaleClient available();

    /// `Server` inherits `Print`; a listener has nothing to write to.
    size_t write(uint8_t) override { return 0; }

  private:
    /// Shared with the detached acceptor, for the same reason `ConnState` is
    /// shared with the connection workers: the acceptor parks inside
    /// `ts_tcp_accept` and must have somewhere valid to deliver into.
    struct AcceptState {
        ~AcceptState();
        ts_tcp_listener *listener = nullptr;
        std::atomic<bool> running{true};
        std::mutex mutex;
        std::deque<std::shared_ptr<tailscale_shim::TailscaleConn>> pending;
    };

    /// Parks in `ts_tcp_accept` and delivers into the shared state. Static so
    /// it can outlive the server object the way a detached thread must.
    static void acceptorLoop(std::shared_ptr<AcceptState> as);

    int port;
    std::shared_ptr<AcceptState> accepting;
};

/**
 * Process-wide tailscale-rs device.
 *
 * Brought up once, before any listener. Phase 1 reads its settings from a
 * plain file rather than from ModuleConfig, so no protobuf or config-schema
 * change is needed yet.
 */
namespace TailscaleTransport
{
/// Bring up the tailnet device. Idempotent; returns true once joined.
bool begin(const char *stateFile, const char *authKey, const char *hostname);
/// The device, or nullptr if `begin()` has not succeeded.
ts_device *device();
/// This node's tailnet IPv4 as a dotted quad, or empty if not joined.
std::string ipv4();
} // namespace TailscaleTransport

#endif // MESHTASTIC_TAILSCALE
