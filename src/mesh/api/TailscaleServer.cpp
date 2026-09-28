// The tailnet listener: joins in the background, then accepts connections for APIServerPort.
#include "TailscaleInternal.h"

#ifdef MESHTASTIC_TAILSCALE

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(ARCH_ESP32) && defined(MESHTASTIC_TAILSCALE_DIAGNOSTICS)
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

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

#endif // MESHTASTIC_TAILSCALE
