# Meshtastic phone API over a tailnet: design notes

This branch serves Meshtastic's normal TCP phone API (port 4403) on the node's tailnet address, using
[tailscale-rs](https://github.com/tailscale/tailscale-rs) through its `ts_ffi` C API. There is no port
forwarding and no public broker, and the LAN API keeps working alongside it.

- **Setting up a node:** [TAILSCALE-QUICKSTART.md](TAILSCALE-QUICKSTART.md)
- **What changed in each version, with results from the board:** [TAILSCALE-CHANGELOG.md](TAILSCALE-CHANGELOG.md)
- **Credits, licences and trademarks:** [NOTICE-TAILSCALE.md](NOTICE-TAILSCALE.md)

This file describes how the code works now. The history of how it got here is in the changelog and in
the commit messages.

## Status

- **Tested on:** Heltec WiFi LoRa 32 V4 (ESP32-S3, 16 MB flash, 2 MB PSRAM), and natively on macOS.
- **On the board (v1.4):** 15 of 15 API sessions succeed over the tailnet. TCP connect takes a median
  of 81 ms and a full config download 606 ms. Free internal RAM stays flat at about 72 KB across dozens
  of sessions, with no reboots.
- **tailscale-rs is beta.** It has no direct connections yet, so all tailnet traffic goes through
  Tailscale's DERP relays.

## How the pieces fit

```
Meshtastic app or CLI ── tailnet (DERP) ──> node:4403
  PhoneAPI / StreamAPI / ServerAPI          stock Meshtastic, unchanged
  TailscaleServerAPI                        src/mesh/api: reads tailscale.conf, owns the port
  Tailscale shim (Client, Server, device)   src/mesh/api: blocking ts_ffi calls -> Arduino Client/Server
  ts_ffi (tailscale.h)                      the tailscale-rs submodule, built by cargo
  tailscale-rs: runtime, control, DERP, netstack, TLS
```

`ServerAPI<T>` and `APIServerPort<T, U>` are already generic over their transport, so
`TailscaleServerAPI` holds no protocol logic: a client on the tailnet talks to the same code as one on
the LAN.

### Where the code lives

| Path | What it does |
| --- | --- |
| `src/mesh/api/TailscaleServerAPI.*` | Reads `/prefs/tailscale.conf` and starts the tailnet API next to the LAN one |
| `src/mesh/api/TailscaleSocket.h` | The shim's interface: `TailscaleClient`, `TailscaleServer`, `TailscaleTransport` |
| `src/mesh/api/TailscaleConn.cpp` | One connection: reader and writer threads, buffers, `TailscaleClient` |
| `src/mesh/api/TailscaleServer.cpp` | Joins in the background, then accepts connections |
| `src/mesh/api/TailscaleTransport.cpp` | The process-wide device: settings, join, teardown |
| `src/mesh/api/TailscaleSocket.cpp` | Shared plumbing: deferred logging, thread stacks |
| `src/mesh/api/TailscaleMbedtls.cpp` | ECDSA certificate checks for tailscale-rs's TLS, on ESP-IDF's mbedTLS |
| `src/platform/esp32/MbedtlsPsram.cpp` | mbedTLS allocations in PSRAM first |
| `src/FSPath.h` | Path checks that keep the node's Tailscale secrets unreadable |
| `tailscale-rs/` | Git submodule: the tailscale-rs commit this firmware needs |
| `extra_scripts/tailscale_link.py` | Finds that checkout, adds `tailscale.h`, links `libtailscalers.a` |

The Tailscale code is behind `MESHTASTIC_TAILSCALE`, and hooks into Meshtastic at startup (`main.cpp`,
`WiFiAPClient.cpp`) and in the template instances. Rust's ESP-IDF support needs ESP-IDF 5, so every
Heltec V4 build on this branch moves from Arduino 2 (ESP-IDF 4.4) to Arduino 3.3 (ESP-IDF 5.5). That
needed the battery ADC port (`Power.cpp`) and the Bluetooth and watchdog changes in `main-esp32.cpp`;
other boards are unaffected. [NOTICE-TAILSCALE.md](NOTICE-TAILSCALE.md) lists every upstream file
changed.

## How the socket shim works

- **Why threads.** ts_ffi's TCP calls only block: there is no poll, no timeout and no "bytes
  available". Meshtastic polls `available()` and `read()` from its cooperative scheduler every ~100 ms
  and must never block, or the radio stalls. So each connection has a reader thread (`ts_tcp_recv`
  into an 8 KB rx buffer; stopping there is the TCP backpressure) and a writer thread (a tx queue into
  `ts_tcp_send`, which may write partially). The scheduler only touches the buffers, under their
  mutexes.
- **`write()` never blocks, and is all or nothing.** StreamAPI writes from two paths (packets and log
  records) and ignores the return value, so a partial frame would corrupt the stream. A write that does
  not fit the 16 KB queue drops the connection instead. TX is double-buffered by swap, not copied.
- **`flush()` is handoff, not delivery,** and waits at most 5 s. StreamAPI calls it after every packet
  on the scheduler thread, so a writer stuck behind a vanished peer must not hold the mesh loop.
- **Who closes the socket.** `ts_tcp_close` consumes the Rust handle while recv and send may still hold
  it, so it runs in `~ConnState`, once both workers and every handle are gone (all hold `shared_ptr`s;
  the workers are detached because a blocked ts_ffi call cannot be interrupted). `close()` only sends a
  FIN (`ts_tcp_shutdown`), so a client the node drops finds out at once.
- **Workers never call `LOG_*`.** Meshtastic's log macros are not thread-safe, so workers queue lines
  and `TailscaleServerPort::runOnce` emits them on the scheduler.
- **Joining happens off the scheduler.** `ts_init`, `ts_ipv4_addr` and `ts_tcp_listen` all block for
  seconds, so a bring-up thread joins, retrying with backoff (5 s doubling to 5 min), then hands over to
  a long-lived accept thread.
- **Buffers live in PSRAM.** `PsramAllocator` keeps them out of internal RAM, which WiFi needs.
- **Tests** drive a connection against a fake peer through the `StreamOps` seam (see
  [Building and testing](#building-and-testing)).

## Running tailscale-rs on an ESP32-S3

The native build exercises none of this; each item cost a boot loop to find.

- **eventfd.** tokio's IO driver needs an eventfd, which ESP-IDF provides only once
  `esp_vfs_eventfd_register()` has run. Without it the runtime fails to start.
- **One tokio worker, not `current_thread`.** With no worker, spawned tasks (control, DERP, netstack)
  only advance inside `block_on`, so the node joins and then never answers. One worker on a dual-core
  chip saves a stack.
- **Rust owns thread stack sizes; C++ owns their placement.** Rust calls `pthread_attr_setstacksize`
  itself, overriding `esp_pthread_set_cfg`'s size, but the embedder still chooses internal RAM or PSRAM.
- **Flash I/O needs an internal-RAM stack.** A task on a PSRAM stack may not disable the flash cache,
  which every flash write does. Only the bring-up thread touches flash (the key state), so every other
  thread runs on PSRAM.
- **Rust's heap is PSRAM first** (`ts_ffi/src/esp_alloc.rs`), and so is mbedTLS (`MbedtlsPsram.cpp`).
- **Low thread priority.** At pthreads' default priority a busy tokio worker starved Meshtastic's loop
  into the 90 s watchdog; the shim's threads run at priority 1.
- **Embedder threads must not call `std::thread::current()`.** On ESP-IDF, each foreign thread that
  does leaks about 350 bytes of internal RAM when it exits (ESP-IDF frees pthread TLS slots before
  std's deferred destructors run). ts_ffi's blocking calls therefore go through tokio's `block_on`, and
  the blocking pool keeps its threads rather than retiring them.
- **TLS certificate checks use mbedTLS.** RustCrypto took about 36 s per handshake, past DERP's 30 s
  limit. The firmware registers mbedTLS's ECDSA with `ts_set_ecdsa_verifier`; tailscale-rs uses it only
  after a known-answer self-test, and only for signature checks, so it can be faster but never more
  permissive. A full DERP handshake takes about 1.7 s on the board.
- **Bluetooth's memory must be claimed at boot.** Arduino 3.3 releases the BLE controller's memory
  unless `bleInUse()` returns true, which NimBLE-Arduino does not arrange. `main-esp32.cpp` claims it,
  then releases it once when Bluetooth goes unused that boot (WiFi on).

### Thread stacks

| Thread | Stack | Where | Notes |
| --- | --- | --- | --- |
| tokio worker, blocking pool (max 2) | 64 KB | PSRAM | ~41 KB peak measured through a join and a TLS handshake |
| Bring-up (join, then exits) | 32 KB | internal | ~17 KB peak; does the key-state flash I/O |
| Accept | 16 KB | PSRAM | Accepts and starts connection threads |
| Per connection: reader, writer | 24 KB each | PSRAM | |

With WiFi and Tailscale up and idle, about 72 KB of internal RAM stays free.

## Security

- **Keys stay out of git.** `bin/ts-set-secret.sh` writes the auth key to `data/prefs/tailscale.conf`,
  which git ignores, after checking its shape; it never echoes it. The first-install zip that carries
  it is written mode 600.
- **Secrets stay on the node.** The node's identity (`/prefs/tailscale_keys.json`) and config
  (`/prefs/tailscale.conf`) are refused by the phone API's file manifest and by XModem, whatever
  spelling of the path is used (`src/FSPath.h` resolves paths the way LittleFS does).
- **Fixes to upstream bugs found on the way** (in all builds): XModem's cancel deleted whichever file
  was last named, and the web server's serve, upload and delete handlers could be escaped with `..`.
  These will be reported to Meshtastic.

## Flash budget

The Heltec builds use the stock two-slot `default_16MB.csv` (6.25 MB app slots), because
flasher.meshtastic.org writes Meshtastic's OTA loader at 0x650000. tailscale-rs adds about 3.9 MB; the
Bluetooth image fills all but about 20 KB of its slot, and the no-Bluetooth image has about 200 KB more.

If the image needs to shrink, in order of value:

1. **`ts_netcheck`, ~250 KB.** STUN probing and latency-based relay choice.
2. **`p384`, ~109 KB.** Needs a crypto provider with per-curve features, and confirmation that
   Tailscale's servers never need P-384.
3. **Not `webpki-roots`** (~250 KB): trimming the root store risks a node that cannot connect after a
   CA change, with no remote fix.

Do not remove HTTP/2: the control connection uses it.

## Building and testing

The quick start has the full build. For development:

```bash
# tailscale-rs's tests
cd tailscale-rs && cargo test

# The firmware's unit tests (macOS): the shim against a fake peer, and the path checks
(cd tailscale-rs && cargo build -p ts_ffi --profile firmware)
pio test -e native-macos-tailscale -f test_tailscale_shim -f test_fspath

# A native tailnet node, and the same under ThreadSanitizer
pio run -e native-macos-tailscale                 # key: tailscale.conf (see tailscale.conf.example)
pio run -e native-macos-tailscale-tsan
```

`TAILSCALE_RS_DIR` points the build at a different tailscale-rs checkout. The build prints the commit
it links, and warns if that is not the one the submodule pins.

Tools in `bin/`:

- `meshtastic-perf-bench.py HOST N`: N timed API sessions over the tailnet.
- `meshtastic-config-timeline.py HOST PORT`: timestamps a config download, to see where a stall starts.
- `native-tailnet-test.sh`: runs the native node against the real control server, with
  `TS_TEST_*` fault injection (tailscale-rs's `test-hooks` feature).

### The Wokwi simulator

`heltec-v4-tailscale-wokwi` boots the firmware in [Wokwi](https://wokwi.com) (`bin/wokwi-run.sh`).
It is useful for boot, memory and threading problems, not for timing: its CPU runs well below real
speed, crypto far slower, and the free plan stops a run after 5 minutes. Wokwi's gateway sees all
traffic, so use a throwaway tailnet and an ephemeral key, and never a board's key
(`bin/wokwi-run.sh` refuses one written with `ts-set-secret.sh tailscale-hw`). Build the filesystem
image with `-t buildfs` rather than by hand: a mismatched LittleFS image is silently reformatted.
Espressif's QEMU does not boot Meshtastic's ESP32-S3 firmware (bootloader PSRAM init stalls).

## Known gaps

- **Relay only.** tailscale-rs has no direct connections yet, and its netstack does not answer ICMP,
  so `tailscale ping --icmp` gets no reply (TCP is unaffected).
- **First join spends ~3 s** on the ECDSA self-test.
- **DERP keepalives are not timed** (an upstream TODO in `ts_derp/src/client.rs`), so a dead relay
  connection is noticed late.
- **Not low power.** WiFi power save is off, and DERP's 60 s keepalive rules out duty cycling: treat a
  tailnet node as mains or solar powered.
- **Bluetooth only runs with WiFi off,** as in stock Meshtastic.
- **Settings are a flat file,** not ModuleConfig, to avoid carrying a protobuf schema change across
  upstream releases.
