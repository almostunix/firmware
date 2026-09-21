#pragma once

#include "ServerAPI.h"
#include "TailscaleSocket.h"

#ifdef MESHTASTIC_TAILSCALE

/**
 * The stock phone API, served over a tailnet connection.
 *
 * There is deliberately nothing here but the type plumbing: every byte of
 * protocol behaviour is inherited from `ServerAPI<T>` / `StreamAPI` /
 * `PhoneAPI`, exactly as the WiFi and Ethernet variants do. That is the whole
 * point of the exercise -- a client reaching the node over Tailscale talks to
 * the same API implementation it would reach over the LAN.
 */
class TailscaleServerAPI : public ServerAPI<TailscaleClient>
{
  public:
    explicit TailscaleServerAPI(TailscaleClient &_client);
};

/**
 * Listens on the node's tailnet address and hands accepted connections to
 * `TailscaleServerAPI`.
 */
class TailscaleServerPort : public APIServerPort<TailscaleServerAPI, TailscaleServer>
{
  public:
    explicit TailscaleServerPort(int port);
};

/**
 * Bring up the tailnet device and start the API server on it.
 *
 * Separate from `initApiServer()` rather than replacing it: the two listen on
 * different network stacks (OS sockets vs. tailscale-rs' userspace netstack),
 * so both can serve port 4403 at once without conflicting, and a node keeps
 * working on the LAN whether or not the tailnet comes up.
 */
void initTailscaleApiServer(int port = SERVER_API_DEFAULT_PORT);
void deInitTailscaleApiServer();

#endif // MESHTASTIC_TAILSCALE
