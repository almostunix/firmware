#include "TailscaleSocket.h"

#ifdef MESHTASTIC_TAILSCALE

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

namespace tailscale_shim
{

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
            LOG_INFO("Tailscale: peer closed connection");
            break;
        }
        if (n < 0) {
            LOG_WARN("Tailscale: recv failed (%d), dropping connection", n);
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

        // `ts_tcp_send` returns once *at least one* byte is written, so a
        // single call is not enough -- loop until the chunk is gone.
        size_t off = 0;
        while (off < chunk.size()) {
            int n = ts_tcp_send(st->stream, chunk.data() + off, chunk.size() - off);
            if (n <= 0) {
                LOG_WARN("Tailscale: send failed (%d), dropping connection", n);
                st->peerUp.store(false);
                break;
            }
            off += static_cast<size_t>(n);
        }

        st->txDrained.notify_all();

        if (!st->peerUp.load())
            break;
    }

    st->txDrained.notify_all();
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

        const size_t room = st->txBuf.size() < TS_TX_BUFFER_LIMIT ? TS_TX_BUFFER_LIMIT - st->txBuf.size() : 0;
        n = std::min(size, room);
        if (n == 0) {
            LOG_WARN("Tailscale: tx queue full, dropping %u bytes", (unsigned)size);
            return 0;
        }
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
        LOG_WARN("Tailscale: server already listening on port %d", port);
        return;
    }

    ts_device *dev = TailscaleTransport::device();
    if (!dev) {
        LOG_ERROR("Tailscale: cannot listen, device is not joined");
        return;
    }

    ts_sockaddr addr = {};
    addr.sa_family = TS_AF_INET;
    addr.sa_data.sockaddr_in.sin_port = static_cast<uint16_t>(port);
    if (ts_ipv4_addr(dev, &addr.sa_data.sockaddr_in.sin_addr) != 0) {
        LOG_ERROR("Tailscale: no tailnet IPv4 assigned yet");
        return;
    }

    ts_tcp_listener *listener = ts_tcp_listen(dev, &addr);
    if (!listener) {
        LOG_ERROR("Tailscale: listen on port %d failed", port);
        return;
    }

    accepting = std::make_shared<AcceptState>();
    accepting->listener = listener;
    std::thread(acceptorLoop, accepting).detach();

    LOG_INFO("Tailscale: API server listening on %s:%d", TailscaleTransport::ipv4().c_str(), port);
}

void TailscaleServer::acceptorLoop(std::shared_ptr<AcceptState> as)
{
    while (as->running.load()) {
        ts_tcp_stream *stream = ts_tcp_accept(as->listener);
        if (!stream) {
            LOG_WARN("Tailscale: accept failed, acceptor exiting");
            break;
        }
        if (!as->running.load()) {
            ts_tcp_close(stream);
            break;
        }

        auto conn = tailscale_shim::TailscaleConn::create(stream);
        LOG_INFO("Tailscale: incoming connection from %s", conn->remoteAddr().c_str());

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

bool begin(const char *stateFile, const char *authKey, const char *hostname)
{
    if (gDevice)
        return true;

    ts_init_tracing();

    // Node identity persists across restarts so the node keeps its tailnet IP;
    // `true` regenerates the file if it is missing or corrupt, which is what
    // makes first boot work without a separate provisioning step.
    ts_persisted_key_state keys = {};
    if (ts_load_key_file(stateFile, true, &keys) < 0) {
        LOG_ERROR("Tailscale: could not load or create key state at %s", stateFile);
        return false;
    }

    // Zero-initialised for defaults -- the header is explicit that leaving
    // this uninitialised is UB.
    ts_config config = {};
    config.hostname = hostname;
    config.key_state = &keys;

    gDevice = ts_init(&config, authKey);
    if (!gDevice) {
        LOG_ERROR("Tailscale: failed to join tailnet");
        return false;
    }

    LOG_INFO("Tailscale: joined tailnet as %s (%s)", hostname, ipv4().c_str());
    return true;
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
