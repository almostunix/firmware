# Tailscale branch: changelog

Versions of the Heltec V4 Tailscale build, newest first. Tagged releases are `heltec-v4-vX.Y` in both
this repository and the tailscale-rs fork. Board results are over a real tailnet, relayed through DERP.
How the code works now is in [TAILSCALE.md](TAILSCALE.md).

## Unreleased

- tailscale-rs's crates are grouped into `crates/` and `tools/`, as upstream's `main` now does. The build
  finds `tailscale.h` in either layout. No change to the firmware's code, and the image is the same size:
  the ESP32 build keeps the embedded source paths in their old, shorter form.

## v1.5 (2026-09-28)

Reorganisation after a code review; same behaviour as v1.4. Reports `2.7.26.c44e6ae`.

- tailscale-rs is now a git submodule (`tailscale-rs/`), so each firmware commit records the
  tailscale-rs commit it needs. `TAILSCALE_RS_DIR` still overrides it, and the build warns when the
  linked checkout differs from the pin.
- The build finds `tailscale.h` and the library in the same checkout (the header path was hard-coded).
- The socket shim is split by job: `TailscaleConn.cpp`, `TailscaleServer.cpp`, `TailscaleTransport.cpp`,
  with shared plumbing in `TailscaleSocket.cpp`.
- The firmware registers its mbedTLS ECDSA verifier with a new `ts_set_ecdsa_verifier`, instead of
  tailscale-rs requiring the firmware to define a symbol at link time.
- tailscale-rs's release profile is upstream's again; the size settings moved to a `firmware` profile
  (`cargo build --profile firmware`). `tailscale.h` no longer declares ESP-IDF's heap functions.
- The Tailscale build environments are marked `board_level = extra`, so upstream's release CI would not
  try to build them.
- This file and a rewritten `TAILSCALE.md` replace the engineering log; comments trimmed to the house
  style; the unused `partition-table-16MB-noota.csv` is gone.
- **Board:** joined the tailnet 10 s after start; the registered mbedTLS verifier passed its self-test in
  2,840 ms. 15/15 sessions, TCP connect median 66 ms, config download median 669 ms. Free internal RAM
  74,264 → 74,132 bytes across about 35 sessions; no reboot.

## v1.4 (2026-09-28)

Clean-up; same behaviour as v1.3. Reports `2.7.26.a906109`.

- Unit tests for the socket shim (8, against a fake peer via the `StreamOps` seam) and for the path
  checks (15).
- Diagnostics and test hooks are opt-in (`MESHTASTIC_TAILSCALE_DIAGNOSTICS`, tailscale-rs's
  `test-hooks` and `diagnostics` features).
- **Board:** 15/15 sessions, TCP connect median 81 ms, config download median 606 ms. Free internal RAM
  72,300 → 72,312 bytes across about 35 sessions; no reboot.

## v1.3 (2026-09-28)

Fixes the per-session RAM leak. Reports `2.7.26.7054460`.

- Each connection thread lost ~350 bytes of internal RAM once it had called into Rust: tailscale-rs's
  blocking calls parked through `std::thread::current()`, whose handle ESP-IDF never frees at thread
  exit. ts_ffi now blocks through tokio's `block_on`, and the blocking pool keeps its threads.
- **Board:** ~0 bytes per session (v1.2: ~700), 15/15 sessions, no reboot, ~73 KB internal RAM free.

## v1.2 (2026-09-25)

v1.1 without `OwnedMutex`, which fixes its crash. Reports `2.7.26.507f0a9`.

- **Board (2026-09-28):** 15/15 sessions, no reboot, but still ~700 bytes of internal RAM lost per
  session.

## v1.1 (2026-09-23): withdrawn

Rebooted every time a tailnet connection closed: `OwnedMutex` destroyed a `std::mutex` that
Espressif's toolchain already destroys (`_GTHREAD_USE_MUTEX_INIT_FUNC`), a double free. Its other
changes carried into v1.2:

- Firmware: `flush()` gives up after 5 s instead of holding the mesh loop (a watchdog reset at 90 s);
  `close()` sends a FIN; connection buffers in PSRAM, TX double-buffered by swap; the log drain stops
  allocating every 100 ms.
- tailscale-rs: netstack TCP keep-alive (30 s) and timeout (120 s), so a vanished peer no longer holds
  its socket and threads forever; 16 KB netstack buffers on ESP-IDF (from 64 KB); control reconnects
  reuse the control server's Noise key (one TLS handshake instead of two); idle connections to other
  DERP regions last 60 s, not 10 s; the control dial plan honours candidate priority (an upstream bug).

## v1.0 (2026-09-23)

First stable build: v0.3, byte for byte. WiFi, Tailscale and Bluetooth, with the secret-file fixes.
Reports `2.7.26.aa49b20`.

- **Board:** TCP connect 84 ms, config download 526 ms (medians of 15), ~65 KB internal RAM free; but
  ~700 bytes lost per API session (fixed in v1.3).

## v0.1 to v0.3 (2026-09-23)

Bring-up on the board.

- **v0.1:** first Bluetooth build; crashed when Bluetooth started.
- **v0.2:** Bluetooth works: Arduino 3.3 had released the BLE controller's memory at boot, because
  NimBLE-Arduino does not claim it. Also the watchdog and log fixes. ~37 KB internal RAM free.
- **v0.3:** internal-RAM diet with no features removed: the accept loop on a PSRAM stack, the bring-up
  thread exits after joining, mbedTLS allocates from PSRAM first.
- **Security:** the phone API's file manifest and XModem refuse the node's Tailscale secrets. Before,
  any API client, on the LAN too, could download the node's private keys with XModem. Also fixed:
  XModem's cancel deleted whichever file was last named, and the web server's file handlers could be
  escaped with `..`.

## Before v0.1

- **2026-09-23, first board boot.** A no-Bluetooth build ran end to end: joined, chose the nearest
  relay by STUN, and served `meshtastic --info` over the tailnet in 4 s. A full DERP TLS handshake took
  1.7 s on the chip (22 s in the simulator with mbedTLS, 36 s with RustCrypto).
- **2026-09-22, the Wokwi simulator.** Found the eventfd, thread-stack, watchdog, USB-serial and
  no-radio problems (see TAILSCALE.md). tailscale-rs gained reconnection to control, relay choice by
  STUN, and a WireGuard fix: the responder's replay check never fired, which stalled the first
  connection after ~2 minutes idle for ~30 s.
- **2026-09-21, native proof.** `meshtasticd` on macOS served the phone API over a tailnet: `--info`,
  `--nodes` and `--sendtext` all worked. Then tailscale-rs cross-compiled for the ESP32-S3: pure-Rust
  TLS, a size-optimised profile, and tokio capped at one worker.
