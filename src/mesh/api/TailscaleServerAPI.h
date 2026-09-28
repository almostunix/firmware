#pragma once

#include "ServerAPI.h"
#include "TailscaleSocket.h"

#ifdef MESHTASTIC_TAILSCALE

/// The stock phone API over a tailnet connection: all protocol behaviour comes from ServerAPI/StreamAPI/PhoneAPI.
class TailscaleServerAPI : public ServerAPI<TailscaleClient>
{
  public:
    explicit TailscaleServerAPI(TailscaleClient &_client);
};

/// Listens on the node's tailnet address and hands connections to TailscaleServerAPI.
class TailscaleServerPort : public APIServerPort<TailscaleServerAPI, TailscaleServer>
{
  public:
    explicit TailscaleServerPort(int port);

  protected:
    /// Emit the workers' queued log lines (LOG_ is only safe here), then accept as usual.
    int32_t runOnce() override;
};

/// Start the tailnet API alongside the LAN one: separate network stacks, so both can use port 4403.
void initTailscaleApiServer(int port = SERVER_API_DEFAULT_PORT);
void deInitTailscaleApiServer();

#endif // MESHTASTIC_TAILSCALE
