// One tailnet TCP connection: its reader and writer threads, and the Arduino Client over it.
#include "TailscaleInternal.h"

#ifdef MESHTASTIC_TAILSCALE

#include "Throttle.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace tailscale_shim
{

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
            st->txPending.wait(lock, [&] { return !st->running.load() || !st->peerUp.load() || !st->txBuf.empty(); });

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
            TS_LOG_ERROR("Tailscale: tx queue full (%u queued, %u needed), dropping connection", (unsigned)st->txBuf.size(),
                         (unsigned)size);
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

#endif // MESHTASTIC_TAILSCALE
