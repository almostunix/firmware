// Unit tests for the tailnet connection shim (src/mesh/api/TailscaleConn.cpp), driven through its
// StreamOps seam against a fake peer instead of a live tailnet. Needs pio test -e native-macos-tailscale.
#include "Arduino.h"
#include "TestUtil.h"
#include <unity.h>

#ifdef MESHTASTIC_TAILSCALE
#include "mesh/api/TailscaleSocket.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using namespace tailscale_shim;

// The peer at the far end of one connection.
struct FakePeer {
    std::mutex m;
    std::condition_variable cv;
    std::deque<uint8_t> toNode; // bytes the peer has sent, not yet received by the node
    std::string fromNode;       // bytes the node has sent
    bool hungUp = false;        // the peer's FIN: recv returns 0 once toNode is drained
    bool stalled = false;       // send blocks, like a peer that stopped acknowledging
    bool gone = false;          // teardown: fail anything still blocked
    std::atomic<int> shutdowns{0};
    std::atomic<int> closes{0};

    void peerSends(const std::string &s)
    {
        std::lock_guard<std::mutex> l(m);
        toNode.insert(toNode.end(), s.begin(), s.end());
        cv.notify_all();
    }
    void peerHangsUp()
    {
        std::lock_guard<std::mutex> l(m);
        hungUp = true;
        cv.notify_all();
    }
    std::string sent()
    {
        std::lock_guard<std::mutex> l(m);
        return fromNode;
    }
};

static FakePeer *peerOf(const ts_tcp_stream *s)
{
    return reinterpret_cast<FakePeer *>(const_cast<ts_tcp_stream *>(s));
}

static int fakeRecv(const ts_tcp_stream *s, uint8_t *buf, uintptr_t len)
{
    FakePeer *p = peerOf(s);
    std::unique_lock<std::mutex> l(p->m);
    p->cv.wait(l, [&] { return !p->toNode.empty() || p->hungUp || p->gone; });
    if (p->toNode.empty())
        return p->gone && !p->hungUp ? -1 : 0;
    size_t n = std::min<size_t>(len, p->toNode.size());
    for (size_t i = 0; i < n; i++) {
        buf[i] = p->toNode.front();
        p->toNode.pop_front();
    }
    return static_cast<int>(n);
}

static int fakeSend(const ts_tcp_stream *s, const uint8_t *buf, uintptr_t len)
{
    FakePeer *p = peerOf(s);
    std::unique_lock<std::mutex> l(p->m);
    p->cv.wait(l, [&] { return !p->stalled || p->gone; });
    if (p->gone)
        return -1;
    p->fromNode.append(reinterpret_cast<const char *>(buf), len);
    return static_cast<int>(len);
}

// A well-behaved peer answers the node's FIN with its own.
static int fakeShutdown(const ts_tcp_stream *s)
{
    FakePeer *p = peerOf(s);
    p->shutdowns++;
    p->peerHangsUp();
    return 0;
}

static void fakeClose(ts_tcp_stream *s)
{
    peerOf(s)->closes++;
}

static bool waitFor(const std::function<bool()> &cond, int ms = 3000)
{
    for (int waited = 0; waited < ms; waited += 5) {
        if (cond())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

static FakePeer *peer;
static std::shared_ptr<TailscaleConn> conn;

void setUp(void)
{
    streamOps = {fakeRecv, fakeSend, fakeShutdown, fakeClose};
    peer = new FakePeer;
    conn = TailscaleConn::create(reinterpret_cast<ts_tcp_stream *>(peer));
}

// Tear down the way the firmware does, and check the socket is freed exactly once.
void tearDown(void)
{
    {
        std::lock_guard<std::mutex> l(peer->m);
        peer->gone = true;
        peer->cv.notify_all();
    }
    conn->close();
    conn.reset();
    TEST_ASSERT_TRUE_MESSAGE(waitFor([] { return peer->closes.load() == 1; }), "socket never freed");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TEST_ASSERT_EQUAL_INT(1, peer->closes.load());
    delete peer;
}

void test_peer_bytes_become_readable()
{
    peer->peerSends("hello");
    TEST_ASSERT_TRUE(waitFor([] { return conn->available() == 5; }));
    TEST_ASSERT_EQUAL_INT('h', conn->peek());
    uint8_t buf[8] = {};
    TEST_ASSERT_EQUAL_UINT(5, conn->read(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("hello", reinterpret_cast<char *>(buf), 5);
    TEST_ASSERT_EQUAL_INT(-1, conn->read());
}

void test_writes_reach_peer_in_order()
{
    TEST_ASSERT_EQUAL_UINT(3, conn->write(reinterpret_cast<const uint8_t *>("abc"), 3));
    TEST_ASSERT_EQUAL_UINT(3, conn->write(reinterpret_cast<const uint8_t *>("def"), 3));
    TEST_ASSERT_TRUE(waitFor([] { return peer->sent() == "abcdef"; }));
}

void test_oversized_write_is_refused_and_drops_the_connection()
{
    std::string big(TS_TX_BUFFER_LIMIT + 1, 'x');
    TEST_ASSERT_EQUAL_UINT(0, conn->write(reinterpret_cast<const uint8_t *>(big.data()), big.size()));
    TEST_ASSERT_FALSE(conn->connected());
    TEST_ASSERT_EQUAL_UINT(0, conn->write(reinterpret_cast<const uint8_t *>("late"), 4));
}

void test_flush_is_prompt_when_the_peer_keeps_up()
{
    conn->write(reinterpret_cast<const uint8_t *>("ping"), 4);
    auto t0 = std::chrono::steady_clock::now();
    conn->flush();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT_LESS_THAN(500, ms);
    TEST_ASSERT_TRUE(conn->connected());
}

void test_flush_gives_up_on_a_stalled_peer()
{
    {
        std::lock_guard<std::mutex> l(peer->m);
        peer->stalled = true;
    }
    conn->write(reinterpret_cast<const uint8_t *>("first"), 5); // the writer takes this and blocks in send
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    conn->write(reinterpret_cast<const uint8_t *>("second"), 6); // queued behind it
    auto t0 = std::chrono::steady_clock::now();
    conn->flush();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT_GREATER_OR_EQUAL(TS_FLUSH_TIMEOUT_MS - 200, ms);
    TEST_ASSERT_LESS_THAN(TS_FLUSH_TIMEOUT_MS + 5000, ms); // bounded, with slack for a loaded host
    TEST_ASSERT_FALSE(conn->connected());
}

void test_close_sends_one_fin_however_often_it_is_called()
{
    conn->close();
    conn->close();
    TEST_ASSERT_EQUAL_INT(1, peer->shutdowns.load());
}

void test_unread_data_outlives_the_peer_hanging_up()
{
    peer->peerSends("bye");
    peer->peerHangsUp();
    TEST_ASSERT_TRUE(waitFor([] { return conn->available() == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST_ASSERT_TRUE(conn->connected());
    uint8_t buf[4];
    conn->read(buf, sizeof(buf));
    TEST_ASSERT_FALSE(conn->connected());
}

void test_reader_stops_at_the_rx_limit_and_resumes()
{
    peer->peerSends(std::string(3 * TS_RX_BUFFER_LIMIT, 'r'));
    TEST_ASSERT_TRUE(waitFor([] { return conn->available() >= TS_RX_BUFFER_LIMIT; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const size_t buffered = conn->available();
    TEST_ASSERT_LESS_OR_EQUAL(TS_RX_BUFFER_LIMIT + 1024, buffered); // one read chunk past the limit at most
    {
        std::lock_guard<std::mutex> l(peer->m);
        TEST_ASSERT_TRUE(!peer->toNode.empty()); // the rest is still with the peer: backpressure
    }
    std::string drained;
    TEST_ASSERT_TRUE(waitFor([&] {
        uint8_t buf[1024];
        drained.append(reinterpret_cast<char *>(buf), conn->read(buf, sizeof(buf)));
        return drained.size() == 3 * TS_RX_BUFFER_LIMIT;
    }));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_peer_bytes_become_readable);
    RUN_TEST(test_writes_reach_peer_in_order);
    RUN_TEST(test_oversized_write_is_refused_and_drops_the_connection);
    RUN_TEST(test_flush_is_prompt_when_the_peer_keeps_up);
    RUN_TEST(test_flush_gives_up_on_a_stalled_peer);
    RUN_TEST(test_close_sends_one_fin_however_often_it_is_called);
    RUN_TEST(test_unread_data_outlives_the_peer_hanging_up);
    RUN_TEST(test_reader_stops_at_the_rx_limit_and_resumes);
    exit(UNITY_END());
}

#else
void setUp(void) {}
void tearDown(void) {}
void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END()); // only meaningful in a MESHTASTIC_TAILSCALE build
}
#endif

void loop() {}
