#include "configuration.h"

#ifdef MESHTASTIC_TAILSCALE

#include "TailscaleServerAPI.h"
#include <Arduino.h>
#include <cstdlib>
#include <fstream>
#include <string>

static TailscaleServerPort *tailscaleApiPort;

/**
 * Phase 1 configuration, read from a file rather than from ModuleConfig.
 *
 * Deliberate: adding a protobuf field means regenerating nanopb sources and
 * carrying a schema change across upstream releases, which is a much larger
 * commitment than this feature needs while it is still being proven. A plain
 * file gets the same information in with nothing to migrate, and the eventual
 * move to ModuleConfig does not change anything above this line.
 *
 * Format is one `key=value` per line: `authkey=`, `hostname=`, `statefile=`.
 */
struct TailscaleSettings {
    std::string authKey;
    std::string hostname = "meshtastic";
    std::string stateFile = "tailscale_keys.json";
};

static bool loadSettings(const char *path, TailscaleSettings &out)
{
    std::ifstream f(path);
    if (!f) {
        LOG_WARN("Tailscale: no config at %s, tailnet API disabled", path);
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        // Tolerate CRLF and trailing blanks -- this file gets hand-edited.
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            value.pop_back();

        if (key == "authkey")
            out.authKey = value;
        else if (key == "hostname")
            out.hostname = value;
        else if (key == "statefile")
            out.stateFile = value;
    }

    if (out.authKey.empty()) {
        LOG_ERROR("Tailscale: config at %s has no authkey=", path);
        return false;
    }
    return true;
}

void initTailscaleApiServer(int port)
{
    if (tailscaleApiPort)
        return;

    const char *configPath = getenv("MESHTASTIC_TAILSCALE_CONFIG");
    if (!configPath)
        configPath = "tailscale.conf";

    TailscaleSettings settings;
    if (!loadSettings(configPath, settings))
        return;

    // Joining blocks until the control plane answers, so this belongs in
    // startup rather than anywhere the scheduler can reach.
    if (!TailscaleTransport::begin(settings.stateFile.c_str(), settings.authKey.c_str(), settings.hostname.c_str()))
        return;

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

#endif // MESHTASTIC_TAILSCALE
