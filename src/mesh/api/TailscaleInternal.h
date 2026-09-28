#pragma once

// Shared by the Tailscale shim's .cpp files; not part of its interface.

#include "TailscaleSocket.h"

#ifdef MESHTASTIC_TAILSCALE

#include <cstddef>

namespace tailscale_shim
{

// Stack sizes in bytes. TS_TOKIO_STACK only sets placement (Rust sets the size); bring-up is internal for file I/O.
constexpr size_t TS_TOKIO_STACK = 64 * 1024;
constexpr size_t TS_BRINGUP_STACK = 32 * 1024; // ~2x measured; freed once the acceptor takes over
constexpr size_t TS_ACCEPT_STACK = 16 * 1024;  // PSRAM: accept + spawning connection threads, no flash I/O
constexpr size_t TS_IO_STACK = 24 * 1024;

/// Stacks for file I/O must be internal: a task on a PSRAM stack may not disable the flash cache.
enum class StackMem { Internal, Psram };

/// Stack for threads the caller creates after this; esp_pthread_set_cfg is per thread and inherited.
void setThreadStack(size_t bytes, const char *name, StackMem where);

/// Undo setThreadStack for the calling thread, for threads that spawn ours but are not ours.
void restoreThreadStackDefaults();

/// Internal-RAM headroom, with MESHTASTIC_TAILSCALE_DIAGNOSTICS. esp_rom_printf, so it survives an abort.
void logInternalHeap(const char *when);

} // namespace tailscale_shim

#endif // MESHTASTIC_TAILSCALE
