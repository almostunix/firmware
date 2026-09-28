// The process-wide tailscale-rs device: settings, joining, and teardown.
#include "TailscaleInternal.h"

#ifdef MESHTASTIC_TAILSCALE

#include <cstdio>
#include <mutex>

#ifdef ARCH_ESP32
#include <esp_rom_sys.h>
#include <esp_vfs_eventfd.h>
#endif

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
    esp_rom_printf("Tailscale: lwIP sockets free before tailnet start: %d\n", countFreeSockets());
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
