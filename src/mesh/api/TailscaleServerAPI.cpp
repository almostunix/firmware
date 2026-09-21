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
    std::ifstream f(path);
    if (!f) {
        LOG_WARN("Tailscale: no config at %s, tailnet API disabled", path);
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Trim before inspecting: this file is hand-edited, so `authkey = x`
        // and an indented comment are both things people actually write. An
        // untrimmed key would silently fail to match, and an untrimmed value
        // would send a leading space to the control plane and come back as an
        // opaque auth rejection.
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

    if (out.authKey.empty()) {
        LOG_ERROR("Tailscale: config at %s has no authkey=", path);
        return false;
    }

    // Catch a truncated or mis-pasted key here rather than as an opaque
    // rejection from the control plane several seconds later. Never log the
    // key itself -- only whether its shape is plausible.
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
        configPath = "tailscale.conf";

    TailscaleSettings settings;
    if (!loadSettings(configPath, settings))
        return;

    // Hand the settings over and let the server's own bring-up thread do the
    // joining. Nothing in here may block: this runs from setup(), and the join
    // can take tens of seconds when the control server is slow or rejects the
    // node -- which previously stalled the radio coming up.
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
