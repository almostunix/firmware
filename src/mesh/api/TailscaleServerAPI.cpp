#include "configuration.h"

#ifdef MESHTASTIC_TAILSCALE

#include "FSCommon.h"
#include "TailscaleServerAPI.h"
#include <Arduino.h>
#include <cstdlib>
#include <string>

static TailscaleServerPort *tailscaleApiPort;

// Settings: /prefs/tailscale.conf, one key=value per line (authkey, hostname, statefile), not ModuleConfig.
/// Rust opens the identity file with std::fs, so on ESP32 it needs LittleFS's VFS mount point.
#ifdef ARCH_ESP32
#define TS_DEFAULT_STATEFILE "/littlefs/prefs/tailscale_keys.json"
#else
#define TS_DEFAULT_STATEFILE "tailscale_keys.json"
#endif

/// The config is read through FSCom, which is rooted at LittleFS on ESP32 and --fsdir on Portduino.

struct TailscaleSettings {
    std::string authKey;
    std::string hostname = "meshtastic";
    std::string stateFile = TS_DEFAULT_STATEFILE;
};

/// Trim ASCII whitespace from both ends.
static std::string trim(const std::string &s)
{
    static const char *ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

static bool loadSettings(const char *path, TailscaleSettings &out)
{
    // FSCom, not std::ifstream: on ESP32 LittleFS is not on the C++ library's path.
    auto f = FSCom.open(path, FILE_O_READ);
    if (!f || f.isDirectory()) {
        LOG_WARN("Tailscale: no config at %s, tailnet API disabled", path);
        if (f)
            f.close();
        return false;
    }

    std::string line;
    while (f.available()) {
        line = std::string(f.readStringUntil('\n').c_str());
        // Hand-edited file: trim, or `authkey = x` would miss and a stray space would fail the join.
        const std::string entry = trim(line);
        if (entry.empty() || entry[0] == '#')
            continue;

        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            LOG_WARN("Tailscale: ignoring line without '=' in %s", path);
            continue;
        }

        const std::string key = trim(entry.substr(0, eq));
        const std::string value = trim(entry.substr(eq + 1));

        if (key == "authkey")
            out.authKey = value;
        else if (key == "hostname")
            out.hostname = value;
        else if (key == "statefile")
            out.stateFile = value;
        else
            LOG_WARN("Tailscale: ignoring unknown setting '%s' in %s", key.c_str(), path);
    }

    f.close();

    if (out.authKey.empty()) {
        LOG_ERROR("Tailscale: config at %s has no authkey=", path);
        return false;
    }

    // Catch a mis-pasted key now rather than as an opaque rejection; never log the key itself.
    if (out.authKey.rfind("tskey-", 0) != 0)
        LOG_WARN("Tailscale: authkey in %s does not start with 'tskey-'; if the join fails, suspect a paste error", path);

    return true;
}

void initTailscaleApiServer(int port)
{
    if (tailscaleApiPort)
        return;

    const char *configPath = getenv("MESHTASTIC_TAILSCALE_CONFIG");
    if (!configPath)
        configPath = "/prefs/tailscale.conf";

    TailscaleSettings settings;
    if (!loadSettings(configPath, settings))
        return;

    // Must not block (this runs from setup()): the server's own bring-up thread does the join.
    TailscaleTransport::configure(settings.stateFile, settings.authKey, settings.hostname);

    tailscaleApiPort = new TailscaleServerPort(port);
    tailscaleApiPort->init();
}

void deInitTailscaleApiServer()
{
    if (tailscaleApiPort) {
        LOG_INFO("Tailscale: deinit API server");
        delete tailscaleApiPort;
        tailscaleApiPort = nullptr;
    }
}

TailscaleServerAPI::TailscaleServerAPI(TailscaleClient &_client) : ServerAPI(_client)
{
    // Reported to clients as a WiFi-attached API: it is a TCP socket over the
    // node's WiFi uplink, which is what the field actually distinguishes.
    api_type = TYPE_WIFI;
    LOG_INFO("Incoming tailnet connection");
}

TailscaleServerPort::TailscaleServerPort(int port) : APIServerPort(port) {}

int32_t TailscaleServerPort::runOnce()
{
    tailscale_shim::drainDeferredLogs();
    return APIServerPort::runOnce();
}

#endif // MESHTASTIC_TAILSCALE
