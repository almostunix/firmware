# Meshtastic phone API over a tailnet

Serves the existing Meshtastic TCP API (port 4403) on the node's tailnet IP,
via [tailscale-rs](https://github.com/tailscale/tailscale-rs)'s `ts_ffi` C API.
No port forwarding, no public MQTT broker.

`ServerAPI<T>` and `APIServerPort<T, U>` are already generic over their
transport, so `TailscaleServerAPI` contains no protocol logic — a client
reaching the node over Tailscale talks to the same API implementation it would
reach over the LAN. PhoneAPI and StreamAPI are untouched.

## v1.0 review and v1.1 fixes (2026-09-23)

v1.0 (tag `heltec-v4-v1.0`) is the build that ran stably on the board. A
line-by-line review of everything this port adds, plus measurements on the
board over the tailnet, found:

**Measured on the board (v1.0).** API session over DERP: TCP connect 84 ms,
config complete 526 ms (median of 15). But internal heap fell ~700 bytes per
API connection and never came back (64.7 KB -> 43.9 KB across ~30
connections); a bare connect-and-close cost ~300 bytes.

**v1.1 crashed on the board, and why.** It rebooted every time a tailnet
connection closed: `assert failed: multi_heap_free ... (head != NULL)` from
`pthread_mutex_destroy` in `~OwnedMutex`. `OwnedMutex` was meant to fix the leak
above on the theory that libstdc++'s `std::mutex` never destroys its pthread
mutex. That is true of the generic `gthr-default.h`, but Espressif's
`os_defines.h` sets `_GTHREAD_USE_MUTEX_INIT_FUNC`, so on this toolchain
`std::mutex` inits and destroys its own lock, and `OwnedMutex` destroyed it
twice. Removed; the leak's cause is still unknown. Lessons: check the exact
multilib's headers, and no fix counts until it has run on the board.

**Fixed in v1.1 (firmware):**
- `flush()` could hold the mesh loop indefinitely: StreamAPI writes a whole
  config download in one pass and relies on it for backpressure, and a writer
  stuck in `ts_tcp_send` behind a vanished phone never returned (watchdog
  reset at 90 s). Now bounded at 5 s, after which the connection is dropped,
  as a stalled lwIP write is on the WiFi API.
- `close()` now sends a FIN (`ts_tcp_shutdown`). Before, a client the node
  dropped (idle timeout, or replaced by a newer connection) stayed "connected"
  until its next write.
- Connection buffers (up to 8 KB rx + 2x16 KB tx) move to PSRAM; malloc put
  them in internal RAM. TX is double-buffered by swap instead of copied.
- The deferred-log drain no longer allocates ~544 bytes every 100 ms (an empty
  libstdc++ deque allocates).
- `POWER_LOG_ADC_READS` dropped from the board build (bring-up only).

**Fixed in v1.1 (tailscale-rs):**
- Netstack TCP keep-alive (30 s) and timeout (120 s). Without them a peer that
  vanishes without FIN/RST held its socket and both shim threads forever
  (`tcp_liveness` tests: with the stock config a recv on a vanished peer never
  ends).
- Netstack TCP buffers 16 KB per direction on ESP-IDF, from 64 KB: each
  connection, and the spare socket every listener keeps, held 128 KB of PSRAM.
- Control reconnects reuse the control server's Noise key: one TLS handshake
  instead of two (~1.9 s of CPU each on the ESP32). Verified live against
  controlplane.tailscale.com: 1.09 s -> 0.52 s natively.
- Non-home DERP connections idle out after 60 s (Go's value), not 10 s, which
  redialed a peer's home region for nearly every burst of traffic.
- Upstream bug: the control dial plan ignored candidate priority (compared the
  best candidate with itself).

**The per-session RAM loss, found (v1.2 measurements, 2026-09-28).** Probing
the board over the tailnet with different session types isolated it: junk
bytes that only the reader thread handles cost ~363 bytes, a single heartbeat
that also makes the writer send cost ~719, and config downloads with or
without the file manifest cost the same as the heartbeat. So each connection
thread lost ~350 bytes once it had called into Rust, whatever the call did.

Cause: the netstack's `*_blocking` calls park via flume, which uses
`std::thread::current()`. That creates a std `Thread` handle per foreign
thread, holding a pthread mutex and condvar that ESP-IDF allocates in internal
RAM. std frees the handle in a deferred second round of TLS destructors,
reading it from a TLS slot without a destructor, but ESP-IDF's pthread TLS
cleanup frees every slot in list order before that round runs. The handle is
never found, never freed. tailscale-rs 4908840 blocks through tokio's
`block_on` instead (its parker is a thread-local with a destructor), and keeps
the blocking pool's threads rather than retiring them every 10 s. Worth
reporting to esp-rs: any foreign thread calling `thread::current()` leaks. `tailscale ping --icmp` gets no
reply (the netstack does not answer echo); TCP is unaffected. Direct
(non-DERP) paths are upstream "coming soon", so all traffic is relayed.

## Status as of 2026-09-22, evening (paused here)

### Proven

**Native build** (`meshtasticd -s` on macOS, against the real control server;
`bin/native-tailnet-test.sh`):

- Join, home relay advertised, and `meshtastic --info` over the tailnet within
  seconds.
- Reconnection, via the `TS_TEST_*` fault-injection hooks: forced dial failures
  recover with 1/2/4/8 s backoff and kameo never gives up; forced dead map
  streams reconnect, with `--info` succeeding 6/6 across 4 reconnects and a
  steady socket count. The production 180 s stream idle limit ran 7 minutes at
  170 s with zero false reconnects.
- Relay selection under the ESP-IDF probe cap: all 28 regions measured, three at
  a time, and the lowest-latency region chosen. With regions 1-3 forced to hang,
  the other 25 are still measured and a relay is advertised at 29 s. Before the
  fix, that case never produced one.

**ESP32-S3 in Wokwi** (`bin/wokwi-run.sh`):

- Boot, 2 MB PSRAM, partition table, LittleFS, WiFi, DHCP, mDNS, stock API.
- The tokio runtime and key state on target; a full control-plane handshake
  (TLS, Noise, HTTP/2); two real joins with addresses assigned.
- Meshtastic's main loop keeps running while Tailscale is wedged: a 5-minute
  join hang, no watchdog reset.
- Battery: the ADC port reads within 17 mV of target, USB-vs-battery inference
  is right, and the low-voltage cutoff fires, including with Tailscale up.
- The reconnection logic on target: failed dials back off 1, 2, 4 s and retry,
  and `ts_ipv4_addr` waits instead of tearing the runtime down.

**Heltec V4 hardware** (flashed with flasher.meshtastic.org, 2026-09-23):

- First flash boot-looped ~5 s after every start, with or without Tailscale:
  `InstrFetchProhibited` at PC 0 inside IDF 5.5's `esp_bt_controller_init`,
  called from NimBLE-Arduino 1.4.3's `NimBLEDevice::init` when PowerFSM turns
  Bluetooth on. NimBLE-Arduino 1.4.3 targets IDF 4.4; the Arduino 3.3 libs
  ship their own NimBLE/controller (`CONFIG_BT_NIMBLE_ENABLED=y`). The zip
  first flashed with Tailscale was built with `MESHTASTIC_EXCLUDE_BLUETOOTH=1`.
  `[heltec_v4_base]` now uses NimBLE-Arduino 2.3.7 with `NIMBLE_TWO` and
  `CONFIG_BT_NIMBLE_EXT_ADV` instead -- the combination m5stack-unitc6l already
  runs on Arduino 3.x, and `NimbleBluetooth.cpp` already carries the 2.x port.
  It builds; it has not run on a board yet (shipped as an update-only zip with a
  no-Bluetooth rollback). Cost: the Tailscale app is back to 6,526,064 of
  6,553,600 bytes (27.5 KB spare), so the next feature needs the flash trims.
- Up to that point everything else came up on the real board: LittleFS, the
  SSD1306 display, the SX1262 (`SX1262 init success`), the L76K GPS, battery
  ADC calibration, and the task watchdog fix.
- **Working end to end (no-Bluetooth build):** 24 min stable on USB before WiFi
  was set; then joined as `meshtastic-heltec-v4` (100.x.y.z), home relay
  (the nearest) chosen by STUN, and `meshtastic --host 100.x.y.z --info` from
  another tailnet machine returned the full config in 4 s. Battery ADC steady
  (3.81-3.86 V, ±5 mV, rising while charging).
- **TLS on the real chip: 1.7 s** for a full DERP handshake (measured:
  `elapsed_ms=1716 kind=Full`, mbedTLS ECDSA) -- against 22 s simulated with
  mbedTLS and 36 s with RustCrypto. Wokwi overstated crypto cost by >10x.
  Config download over the tailnet after that: ~0.2 s.
- **Internal RAM diet (no features removed).** Measured internal free with
  WiFi+Tailscale: v0.1 ~22-31 KB, v0.2 ~37 KB (bring-up stack 48->32 KB). v0.3
  (built, not yet on the board): the accept loop moves to a PSRAM-stacked
  thread and the bring-up thread exits (+32 KB); mbedTLS allocates PSRAM-first
  via `-Wl,--wrap=esp_mbedtls_mem_calloc` (`src/platform/esp32/MbedtlsPsram.cpp`;
  the original symbol disappears from the image, so every caller is wrapped), and
  the web server's HTTPS gate checks PSRAM accordingly. Next candidates: lower
  `malloc`'s internal threshold with `heap_caps_malloc_extmem_enable` (default
  4096); then a custom-sdkconfig rebuild of the platform libs (WiFi static
  buffers, IRAM placement, BSS in PSRAM).
- Harmless, to clean up: `task_wdt: esp_task_wdt_reset(707): task not found`
  once at boot, a leftover of the watchdog reconfigure.
- **Bluetooth root cause (both NimBLE versions):** Arduino 3.3's `initArduino()`
  releases the BLE controller's ~36 KB to the heap at boot unless `bleInUse()` is
  true; Arduino's own BLE library claims it, NimBLE-Arduino doesn't. Starting BLE
  later made the controller write its ROM data/bss (`btdm: bss start 0x3fcef180`)
  into heap-owned memory: a null call with 1.4.3, a TLSF assert in
  `r_llm_env_init`'s malloc with 2.3.7. Fix (`main-esp32.cpp`): a strong
  `bleInUse()` returning true, then `setBluetoothEnable()` releases the memory
  once when BLE goes unused this boot (WiFi on), so WiFi+Tailscale keeps the RAM.
  Arduino's `btMemReleased()` can't be used: it only tracks releases via linker
  `--wrap` flags this PlatformIO build doesn't pass. **Works on the board (v0.2):**
  with WiFi off the phone pairs and connects; with WiFi back on the node rejoins
  the tailnet (same IP) and `--info` works. Note: an open client.meshtastic.org
  serial session puts the node in PowerFSM's SERIAL state, which keeps Bluetooth
  off by (stock) design -- close it before testing Bluetooth.
- **Bluetooth build flashed via the flasher's Update (2026-09-23):** node kept its
  identity (same 100.x.y.z), rejoined, `--info` OK, `hasBluetooth=true`.
  Bluetooth itself not yet exercised (it only starts with WiFi off).
- **Security, fixed and verified on the board over the tailnet:** the manifest no
  longer lists the Tailscale files (7 entries, was 9) and XModem reads of
  `/prefs/tailscale_keys.json`, `/static/../prefs/tailscale_keys.json` and
  `//prefs/./tailscale.conf` are refused (NAK). Before the fix, any API
  client -- the WiFi-LAN TCP API on 4403 included -- could download
  `/prefs/tailscale_keys.json` through Meshtastic's XModem file transfer, which
  opens any path by name. Also found on the way, both upstream bugs: XModem's
  cancel removed the last-named file unconditionally, so "request any name,
  then cancel" deleted it; and the web server's serve, upload and delete
  handlers built `"/static/" + request` (delete: `"/" + request`), escapable with
  `..`, which littlefs resolves. Fix: `src/FSPath.h` resolves paths as littlefs
  does; XModem and the config file manifest refuse `/prefs/tailscale*`
  (Tailscale builds); cancel removes only a partial upload; the three web
  handlers are confined to `/static` (all builds). Tested: helper unit test
  (22 cases), and on the native daemon every read/write/traversal/cancel attack
  on a fake key file is refused and leaves it intact, while ordinary reads work.
  The web handlers are LAN-only and not exercised yet.

### Open, in priority order

1. **The relay handshake cannot be shown in Wokwi; it needs the board.**
   Certificate checks now go to ESP-IDF's mbedTLS (tailscale-rs `6dc039a`,
   firmware `src/mesh/api/TailscaleMbedtls.cpp`). On the simulated chip the
   known-answer self-test passes and real chains verify: 4/4 control
   handshakes, 22.1-22.2 s each, down from 36 s. But Wokwi runs at ~60% of
   real time (212 s simulated in 345 s), and servers time out in real time,
   so 22 s simulated is ~35 s to a DERP server, which resets at 30 s
   (22.08 s). The simulator is also implausibly slow at this maths:
   the self-test's 9 mbedTLS verifications took 36 s, where the same library
   on real ESP32-class silicon takes tens to hundreds of ms per signature. So
   Wokwi can no longer answer this; the board can. Even if the simulator's CPU
   timing were exact, the handshake is 22 s real time on hardware, under 30.
   - Earlier attempt, kept for the record: `opt-level = 3` for the RustCrypto
     curve crates. 2x faster natively, slower in Wokwi (44-45 s), +141 KB.
     Reverted.
   - TLS sessions now resume (one shared rustls config). DERP reconnects
     benefit; control does not -- `controlplane.tailscale.com` resolves to
     several servers that evidently do not share ticket keys (full handshakes
     natively too).
   - Measured stacks, through the join and a full handshake: tokio worker
     ~40.6 KB of 64 KB; bring-up thread 16.9 KB of 48 KB (internal RAM --
     could shrink to 32 KB and free 16 KB, once the accept loop has been
     measured too).
2. **Relay selection works on the ESP32.** tailscale-rs picked its home region
   by a full TLS handshake to every region (~28); on one tokio worker those
   interleaved, all failed, and starved the control connection until it died.
   Now STUN-first, as the Go client does, with HTTPS (one at a time) only if
   no region answers UDP. In Wokwi: 26 regions answered within 6 s of the
   join, the nearest region chosen, and this Mac's netmap showed the node's relay -- a
   first for the ESP32. Also fixed on the way: an `unwrap()` that crashed the
   chip when the relay handshake failed, and reconnects wasting a backoff step
   on each IPv6 dial-plan address (the ESP32 has no IPv6 route). Earlier
   "control dials fail since the stderr change" (runs 3-4) was environmental:
   the same firmware joined later, and plain lwIP connects to control and to
   DERP (by name and by IP) all succeeded in a diagnostic run.
3. ~~The first `meshtastic --info` after ~2 minutes idle times out.~~ **Fixed
   2026-09-22** (tailscale-rs `d192047`). A WireGuard bug: the responder's
   replay check never fired (`last_seen_timestamp` was never written, and it
   compared with `<`). A peer that has stopped trusting its best path sends a
   rekey initiation over DERP *and* direct UDP, so it arrives twice; the node
   answered both, discarded the session the peer had completed on, and dropped
   every data packet as "session not found" for ~30 s. Measured first byte
   after 130 s idle: 31.8-33.3 s before, 0.08 s after. Matters most on real
   hardware, where a phone on the same WiFi has both paths. It was also a
   replay DoS. Diagnosed with `bin/meshtastic-config-timeline.py`, which
   timestamps the config download instead of reporting pass/fail.
4. Stack sizes are budget-fitted, not high-water-mark measured
   (`uxTaskGetStackHighWaterMark`), especially through a TLS handshake.
5. Battery accuracy against a real cell: check the reported voltage with a
   multimeter, on USB with no cell, before trusting the cutoff.
6. ~~The OTA partition table.~~ **Done 2026-09-23:** `heltec-v4-tailscale` is
   back on stock `default_16MB.csv`, because flasher.meshtastic.org writes the
   OTA loader at 0x650000, which lands inside the app on the single-slot
   layout. The app is 6,513,584 of 6,553,600 bytes (99.4%), so any growth
   needs the trims under "Flash budget". Still open: the socket budget -- 12
   of lwIP's 16 are free when Tailscale starts, with Meshtastic's two web
   servers the obvious candidates if more are needed.

Wokwi minutes spent: see `.pio/wokwi-logs/ledger.tsv` (74.4 min over 13 runs as
of this writing; 9.4 of those went on one crash loop before the harness
learned to catch resets).

### Secrets left on disk

The simulator's auth key is in `data/prefs/tailscale.conf` and in every
filesystem and flash image built since (under `.pio/`). All gitignored. Delete
the key in the Tailscale admin console when done; the ephemeral
`meshtastic-wokwi*` and `meshtastic-native-test*` nodes remove themselves.

## Phase 1 verification (2026-09-21)

Verified end to end on 2026-09-21: `meshtastic --host 100.x.y.z:4403 --info`,
`--nodes` and `--sendtext` all work against `meshtasticd -s`, over DERP, with
each connection accepted and closed cleanly.

Re-verified after the Phase 2 changes — pure-Rust TLS (oxitls-rustcrypto-provider),
`opt-level = "z"`, `codegen-units = 1`, `panic = "abort"`. A pure-Rust provider
does complete a real handshake against Tailscale's control plane and a DERP
relay; that could not be assumed from a successful build. Read and write paths
were clean over the tailnet across repeated runs.

Two operational findings worth keeping:

- **A persisted `tailscale_keys.json` re-authenticates without spending an auth
  key.** After the single-use Phase 1 key was consumed, stopping the node and
  restarting it from the same working directory rejoined the same tailnet IP in
  about a second, with no registration round-trip. Keep that file and testing
  does not need a fresh key each run.
- **`--sendtext` prints `Broken pipe` -- a race inside the Python CLI, not
  this code.** Corrected 2026-09-22; the original note here had it backwards.
  Counting the error text rather than exit codes: 8/8 on loopback with
  Tailscale *disabled*, 7/8 with it enabled, 3/8 over the tailnet, and every
  run exits 0 with the message delivered. When the config download completes,
  the CLI's reader thread starts a heartbeat and sends it at once; for a
  one-shot command the main thread has already half-closed the socket
  (`shutdown(SHUT_WR)` in `TCPInterface.close`), so that write gets EPIPE. The
  faster the path, the more often the heartbeat loses, which is why loopback
  looked worse, not better. Harmless; an upstream CLI issue if anyone wants to
  report it.

## Building

The Rust side is not driven by PlatformIO — build it first:

    cd ../tailscale-rs-0.6.1 && cargo build -p ts_ffi --release
    pio run -e native-macos-tailscale

Then create `tailscale.conf` (see `tailscale.conf.example`) and run:

    .pio/build/native-macos-tailscale/meshtasticd -s

`native-macos-tailscale-tsan` is the same build under ThreadSanitizer. Use it
whenever the shim's threading changes — it has already caught one real race.

## Size baseline

Measured on the Phase 1 build, as a reference point for the Phase 2 size work.
These are macOS ARM64 numbers and do **not** predict Xtensa; they exist so the
reduction is measurable against something rather than re-derived later.

| build | file | `__text` (machine code) |
| --- | --- | --- |
| `native-macos` (stock) | 2,642,632 | 1,075,340 |
| Phase 1: aws-lc-rs, `opt-level = "s"` | 17,839,768 | 7,972,984 |
| \+ pure-Rust TLS (rustcrypto) | 17,087,368 | 7,600,632 |
| \+ `opt-level = "z"`, `codegen-units = 1`, `panic = "abort"` | 13,014,712 | 4,242,772 |
| \+ `worker_threads(1)` | — | 4,241,456 |
| \+ regex dropped from `ts_netmon` | — | **4,004,804** |

So Phase 2 has taken the added code from **+6.6 MB down to +2.79 MB**, a 50%
cut in `__text`. Against `default_16MB.csv`'s 6.25 MB app partition — which
also has to hold Meshtastic's own ~1 MB — that now fits with room to spare,
where the Phase 1 figure did not.

The attribution is worth recording, because it is not what was expected:

- **the cargo profile is the dominant lever** (-3.36 MB, 44%). Note the profile
  was *already* `lto = true, opt-level = "s", strip = true` — the win came from
  going further to `"z"` plus one codegen unit and `panic = "abort"`, not from
  turning on optimisation that was missing.
- **replacing aws-lc-rs is nearly irrelevant to size** (-372 KB, 4.7%). It is
  required because aws-lc-rs cannot build for Xtensa, but it should not be sold
  as a size measure.

The provider is `oxitls-rustcrypto-provider` 0.3.0, not the better-known
`rustls-rustcrypto`. The latter is still 0.0.2-alpha and unpublished since
April 2024, and it pins `rustls-webpki ^0.102` where this tree resolves
0.103.15 — so it linked two copies of the certificate parser. oxitls has no
webpki dependency. Swapping to it removed the duplicate and changed `__text` by
+108 bytes, i.e. not at all: LTO was already dead-stripping the unused copy, so
this is an advisory-surface win rather than a size one.

### On the remaining two "Xtensa constraints"

Both turned out to be largely answered already, and one of them was wrong.

**`current_thread` tokio: do not.** It does not work here and it fails in a way
that looks like success — the node joins, `ts_tcp_listen` returns a listener,
the log says it is serving, and every connection times out. With no worker,
spawned tasks (control-plane poll, DERP, netstack) only advance while a thread
sits inside `block_on`, and ts_ffi's blocking API returns as soon as its own
future resolves. Making it work needs a driver thread parked on the runtime plus
reworking all 8 `block_on` sites to dispatch `'static` futures over a channel —
and that driver thread costs exactly the one thread `worker_threads(1)` costs,
so it buys no RAM. It buys 9 KB of flash (0.2%). `worker_threads(1)` is in
place instead: one thread stack rather than two on a dual-core S3, no redesign.
`thread_stack_size` is the next RAM lever and wants tuning against real
high-water marks on the board.

**Dropping ts_netcheck / ts_netmon / ts_transport_tun: mostly already true on
the target.**

- `ts_transport_tun` is not in the `ts_ffi` dependency graph at all. Nothing to
  remove; it only ever arrived via `ts_cli_util`.
- `ts_netmon` is already inert on ESP-IDF: `platform_mon()` is
  `#[cfg(any(windows, target_os = "linux", target_os = "macos"))]` and returns
  `None` elsewhere, so `ts_runtime` never starts the actor. Its expensive part
  was `#[cfg(target_os = "macos")] pub mod bsd`, which does not compile for
  Xtensa. The regex bloat it carried was therefore a **macOS-only artifact** —
  removed above, which also makes these reference numbers more predictive of the
  Xtensa build rather than less.
- `ts_netcheck` is the only genuine lever left, and it is not cheap. It is live
  (392 symbols), platform-independent, and removing it threads through 25
  references across five `ts_runtime` files including `control_runner.rs`
  (control-plane registration) and `multiderp/uniderp.rs`, plus `NetInfo`
  reporting in `ts_control_serde`. It also costs function: no STUN probing and
  no latency-based DERP region selection. Deferred deliberately — at +2.79 MB
  against a 6.25 MB partition the space is not needed, and the right time to
  judge it is against a real Xtensa build.

A further size lever, not taken: `tracing-subscriber`'s `env-filter` feature
pulls `matchers` -> `regex-automata` (~1,300 symbols still present).

Measure with a forced relink, or trust nothing — see the note in
`extra_scripts/tailscale_link.py`.

## Phase 2: Xtensa cross-build

`ts_ffi` — and with it the whole tailscale-rs stack: tokio, rustls, DERP,
smoltcp — **compiles for `xtensa-esp32s3-espidf`**. Verified: the archive holds
ELF32 objects with `e_machine` 0x5E (Xtensa).

    espup install --std --targets esp32s3 --toolchain-version 1.98.1.0
    . ~/export-esp.sh
    cd ../tailscale-rs-0.6.1
    RUSTUP_TOOLCHAIN=esp cargo build -p ts_ffi --release \
      --target xtensa-esp32s3-espidf -Zbuild-std=std,panic_abort

Notes that matter:

- esp-rs ships Xtensa Rust **1.98.1.0**, matching this workspace's pinned
  `1.98.1` exactly. No version conflict, which was the main toolchain risk.
- There is no prebuilt std for the target, so `-Zbuild-std` plus the `rust-src`
  component is required. `RUSTUP_TOOLCHAIN=esp` is needed to override
  `rust-toolchain.toml`.
- `mio` supports ESP-IDF: `espidf` is excluded from the epoll/kqueue selectors
  and falls through to the `poll(2)` one, so tokio's networking has a path.
  This was the risk most likely to make the whole approach a dead end.
- Two source fixes were needed, both committed in the tailscale-rs fork:
  dropping tokio's `"full"` feature set (its `signal` feature pulls
  signal-hook-registry, which ESP-IDF's libc cannot satisfy), and gating
  `ts_netmon`'s `PlatformMon` re-export, which was unconditional while the type
  is only defined for macos/linux/windows.

**Code size on Xtensa is still unknown.** With `lto = "fat"` the archive members
are `.llvmbc` bitcode, so codegen happens at link time — the 19.6MB archive size
says nothing about firmware size. The real number needs a link against ESP-IDF,
which needs the platform bump below.

### Next: the ESP-IDF platform bump

`variants/esp32/esp32-common.ini` pins `platformio/espressif32@6.13.0`, which is
Arduino core 2.0.17 on **ESP-IDF 4.4.7**. `esp-idf-sys` treats 4.4 as legacy and
is tested against 5.1–5.5, so a bump is on the critical path regardless of which
emulator rung comes next.

There is in-repo precedent to follow rather than invent:
`variants/esp32c6/esp32c6.ini` already replaces the pin with Jason2866's
`platform-espressif32` (pioarduino) at a pinned commit, plus a framework package
built on `release_v5.1`.

## Flash budget, and the OTA requirement for production

Measured on the real Xtensa link (not estimated from the host, which was 41% low):

| | bytes | |
| --- | --- | --- |
| stock `heltec-v4` | 2,226,741 | Meshtastic + IDF alone |
| `heltec-v4-tailscale` | 6,346,337 | **+3.93 MB** of Rust |

`heltec-v4-tailscale` currently uses `partition-table-16MB-noota.csv`, a single
12.5 MB app slot, which puts it at 48%. **That layout has no OTA and is for
bring-up only.**

**Production needs OTA, but NOT `default_16MB.csv`.** An earlier version of this
file claimed the symmetric 6.25/6.25 split was required so an update could be
written to the inactive slot. That was wrong. Meshtastic does not keep a second
full firmware copy: `src/platform/esp32/MeshtasticOTA.cpp` looks up
`ESP_PARTITION_SUBTYPE_APP_OTA_1` and boots a small dedicated OTA loader
(`bin/device-install.sh` flashes `mt-<mcu>-ota.bin` there), which then writes the
real firmware into `ota_0`. The project's own 4 MB table makes the intent plain:
`app` is 0x250000 while `flashApp` is only 0x0A0000 (640 KB) — deliberately
asymmetric. The symmetric 16 MB split is an inherited Arduino-core default.

So an asymmetric 16 MB table — roughly `ota_0` ~11 MB, `ota_1` ~1 MB, spiffs
3.5 MB — keeps OTA fully working with megabytes to spare. What it gives up is
A/B rollback, which Meshtastic does not use on any board.

For reference, the firmware does also fit the stock symmetric table:
6,346,337 of 6,553,600 is 96.8%, leaving 207,263 bytes.

One interaction to keep in mind, because it is not obvious:

> **`RUST_LOG` and OTA conflict only on the stock symmetric table.** Restoring
> `tracing-subscriber`'s `env-filter` costs 243 KB, which would put the firmware
> at 6,589,105 — over a 6,553,600 slot. On an asymmetric table (above) there is
> room for both and the trade disappears.

Trims identified, with attribution, in order of value per unit of mess:

1. **`ts_netcheck`, ~250 KB.** Uniquely pulls `stun-rs` -> `precis-profiles` ->
   `precis-core` (45 KB of Unicode tables, almost all data) plus the
   `stunner`/`direct` kameo monomorphizations (~35 KB). Costs STUN probing and
   latency-based DERP region selection. 25 references through
   `ts_runtime/control_runner.rs`, `multiderp/uniderp.rs` and `NetInfo`.
   **This one alone would pay for both OTA margin and `RUST_LOG`.**
2. **`p384`, 109 KB in 27 symbols.** oxitls exposes no per-curve features, so it
   means a fork edit or moving to `rustls-rustcrypto-gated`, whose selling point
   is exactly algorithm feature gating. Confirm Tailscale's servers never
   negotiate P-384 first.
3. **`webpki-roots`, ~250 KB for 118 roots — recommended against.** The crate
   exposes one array with no per-root symbols, so trimming means hardcoding a
   subset in the fork. If Tailscale rotates CAs, a field node stops connecting
   with no remote fix. Not worth it.

Where the code actually goes, by crate (top of 5.11 MB `.flash.text`): C/C++
1,954,768 · kameo 552,383 · core 437,458 · tokio 391,316 · ts_runtime 177,452 ·
alloc 139,079 · rustls 124,117 · p384 109,444 · h2+hyper+http 144,644 ·
serde_json 68,268 · curve25519_dalek 67,858 · precis_core 46,689.

`kameo` is the largest Rust crate but is the architecture — its size is
per-actor monomorphization, so the only lever on it is removing actors, i.e.
item 1.

**Do not try to remove h2/HTTP-2.** It reads as vestigial — the workspace asks
for hyper's `http2` while `ts_http_util` requests only `http1` — but
`ts_control/src/control_dialer.rs:262` calls `ts_http_util::http2::connect()`.
HTTP/2 is how the node reaches the control plane. Removing it (-145 KB) breaks
the join; this was tried and reverted.

## Note: tailscale-rs already has an SSH server

`tailscale-rs/src/ssh/` wraps **russh 0.60** behind the crate's `ssh` feature,
with `Device::serve_ssh()` and an `ssh_peer_lookup` example. Two things stop it
building here as shipped: `Cargo.toml:236` selects russh's `aws-lc-rs` backend
(which cannot build for Xtensa — the same reason `ts_tls_util` had to move to a
pure-Rust provider), and `ssh = [..., "dep:ratatui"]` pulls ratatui -> crossterm
-> signal-hook-registry, the crate already documented above as unbuildable for
ESP-IDF. Both are small edits in a repo we already fork.

Worth knowing before anyone plans SSH work: it is not a from-scratch port.
Sizing, and the reasons to be cautious about it, are in the scoping notes.

## Local emulation on an arm64 Mac: QEMU findings

Espressif publishes a prebuilt `qemu-xtensa-softmmu` for **aarch64-apple-darwin**
(release `esp-develop-9.2.2-20260417`, ~4 MB). It runs locally with no account,
no token and nothing observing the traffic, which makes it strictly preferable to
Wokwi for anything involving a real tailnet key. It needs
`brew install pixman libgcrypt sdl2`, and `qemu-system-xtensa -machine help`
confirms an `esp32s3` machine.

**It does not currently boot Meshtastic's ESP32-S3 firmware**, and this is not
caused by anything in this integration: stock `heltec-v4`, with no Tailscale
compiled in at all, stops at the identical point. Both reach the ROM loader and
hand off to the second-stage bootloader, which then prints nothing:

    ESP-ROM:esp32s3-20210327
    rst:0x1 (POWERON),boot:0x4 (SPI_FLASH_BOOT)
    load:0x403cb700,len:0x30b0
    entry 0x403c88b8          <- silence from here

`-d guest_errors,unimp` shows why: `M25P80: Unknown cmd 5a / 7a / 77` — Read-SFDP
and quad-PSRAM opcodes arriving at QEMU's generic SPI flash model, i.e. PSRAM is
not being emulated on that bus, and bootloader PSRAM init stalls. Console routing
was ruled out first (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, so the bootloader does
use UART0), as was the `-m 2M` PSRAM flag, which is correct per Espressif's docs.

Getting further would mean a PSRAM-disabled build variant, which diverges from the
board and which the Rust heap will eventually contradict anyway.

And networking would still be missing: `CONFIG_ETH_USE_OPENETH is not set` in
pioarduino's Arduino 3.3.12 precompiled libs for esp32s3. The ESP-IDF >= 5.3 floor
for openeth-on-a-non-EMAC-chip **is** cleared by the 5.5.5 bump, so that is no
longer the blocker — but flipping a Kconfig in precompiled Arduino libs means
rebuilding the whole library set.

Net: QEMU is the right *local* tool and is installed, but booting Meshtastic in it
is its own project. Wokwi (cloud, token, throwaway tailnet) or hardware remain the
practical routes.

## Running it in Wokwi

Wokwi is the cloud simulator; `wokwi-cli` is at `~/.local/bin` (v0.27.1, arm64).
Two secrets are needed and neither may be pasted into a chat or a shell command:

    bin/ts-set-secret.sh wokwi       # Wokwi CI token, from wokwi.com/dashboard/ci
    bin/ts-set-secret.sh tailscale   # ephemeral, single-use tailnet auth key

Both prompt with echo off, validate before writing, write mode 600, and report
only a length -- never a value. Both destinations are gitignored.

Use the script rather than a `read -s` one-liner. The one-liner was tried first
and failed twice in one sitting: a `pbpaste` route wrote the *command text* into
.wokwi-env, and the Wokwi token was typed at the Tailscale prompt. Neither was
caught at the time, because an invisible prompt gives no feedback and nothing
checked the result. The script rejects both: it requires `tskey-` for a tailnet
key, rejects a `tskey-` value for the Wokwi token, and rejects anything
containing a character outside [A-Za-z0-9._-].

That last check is a safety property, not a nicety. .wokwi-env is *sourced*, so
a value containing `;`, `&`, `>` or `$( )` is arbitrary code execution at the
point of use. The token is also written single-quoted for the same reason.

Then:

    pio run -e heltec-v4-tailscale-wokwi -t buildfs   # config -> LittleFS image
    pio run -e heltec-v4-tailscale-wokwi              # app + merge + stage
    . ./.wokwi-env && ~/.local/bin/wokwi-cli --timeout 180000 .

### Why firmware.bin is a full 15.9MB flash image

`wokwi-cli` writes a single blob at offset 0 and has no option for extra flash
regions. The auth key lives in `/prefs/tailscale.conf` on LittleFS, so
`extra_scripts/wokwi_stage.py` merges the filesystem image into the factory
image at the `spiffs` offset, padding the gap with 0xFF (erased NOR flash).

The offset is read from the partition table *inside the factory image* rather
than from the CSV, so it cannot drift from the table the device actually boots.
Verified: superblock at 0xC90000, `authkey=` at 0xCD4020, total 0xFF0000.

If Wokwi rejects a 15.9MB upload, the fix is a simulator-only partition table
that places `spiffs` immediately after the app (~6.8MB total) -- not a change to
the merge logic.

### Do not hand-build the LittleFS image

PlatformIO's `tool-mklittlefs` is an x86_64 binary and will not exec directly on
an arm64 Mac ("bad CPU type"), though it runs fine under the build. Do not
reach for `littlefs-python` instead: the framework ships precompiled libs with
no `lfs.h`, so the on-disk parameters are not discoverable from source, and a
mismatched superblock mounts as corrupt and is silently reformatted -- which
looks exactly like a missing config while burning a simulator run and a
single-use key. Use `-t buildfs` and let the toolchain do it.

For the record, read out of a generated superblock: disk version 0x00020001,
block_size 4096, block_count 864, name_max 64 (not the 255 default), file_max
0x7FFFFFFF, attr_max 1022.

### What the simulator actually found

Five real defects, in the order they surfaced. Every one of them would have
presented on hardware, and three would have been hard to attribute there.

1. **A 15.9MB flash image stalls wokwi-cli.** No clean rejection -- the
   transport closes (code 1006) or the upload hangs. 6.5MB is fine. Fixed by
   partition-table-wokwi.csv.

2. **The task watchdog boot-loops the node.** IDF 5.5.5 sets
   CONFIG_ESP_TASK_WDT_INIT=y, so the TWDT is already running before
   esp32Setup(). esp_task_wdt_init() returns ESP_ERR_INVALID_STATE and the
   assert(res == ESP_OK) panics. This came in with the IDF 5 re-gating: the
   compile error was fixed without noticing the runtime contract had changed.
   Now reconfigures the running instance. On a board this is a node that
   reboots forever over USB serial with Tailscale as the obvious wrong suspect.

3. **Serial went to USB CDC, which the simulator does not emulate.** The board
   definition forces -DARDUINO_USB_CDC_ON_BOOT=1; diagram.json wires the
   monitor to UART0. Every Meshtastic log line was invisible, and Arduino's
   printAfterSetupInfo() crashed writing through it (IllegalInstruction at a
   DROM address). Simulator builds pin Serial to UART0.

4. **No LoRa radio in the simulation blocks setup() before networking.**
   RadioLib waits on a BUSY line that never settles, so initWifi() -- and
   therefore initTailscaleApiServer() -- is never reached. MESHTASTIC_SIMULATOR_NO_LORA
   returns early from initLoRa(); main() already treats a null interface as a
   recorded critical error and carries on. SimRadio is not an option, it is
   compiled only for ARCH_PORTDUINO.

5. **Thread stacks.** The big one. See the tokio commit in tailscale-rs and
   setThreadStack() in TailscaleSocket.cpp. Rust wants 2 MiB per thread and
   ESP-IDF pthreads get 2048 bytes; neither number is survivable.

Confirmed working on the target as a result: boot, 2MB quad PSRAM, the
partition table, LittleFS mount and config read, WiFi association with
Wokwi-GUEST, DHCP (10.13.37.2), mDNS, and the stock API listening on 4403.

### Where Wokwi stops being useful

The CI plan caps a simulation at 5 minutes of wall clock, and this firmware
needs ~145 seconds of *simulated* uptime to reach WiFi on Wokwi's throttled
CPU. Runs land at 144-148s uptime when the cap hits -- the tailnet join is just
past the edge, and which side of it a run falls on is decided by load on
Wokwi's shared infrastructure, not by anything in the firmware.

So the join itself was never exercised here -- at the time. It later was: see the status section at the top. Do not spend more time on it:
hardware has a real SX1262, no time cap, no throttling, and no monitored
gateway -- which also means the auth key never crosses an intermediary.

### Sequence the runs to protect the key

An ephemeral key is single-use. Run once with a deliberately invalid key first:
an auth rejection from the control plane proves boot, WiFi association, config
parsing and the join attempt all work, and costs nothing. Only then write the
real key.

## Running on ESP32 hardware

Build and flash (the board must be on the machine running these; a VM without
USB passthrough cannot):

    bin/ts-set-secret.sh tailscale        # writes data/prefs/tailscale.conf
    pio run -e heltec-v4-tailscale -t uploadfs   # LittleFS, carries the key
    pio run -e heltec-v4-tailscale -t upload     # application
    pio device monitor -b 115200

uploadfs must come first on a fresh board, or the node boots with no config and
reports the tailnet API disabled.

Current size: 6,350,857 bytes, 48.5% of the 12.5MB single-app slot. Note that
this table has no OTA -- see the flash budget section.

### What the ESP32 needs that the host does not

Four things, none of which the native build exercises:

1. **An eventfd VFS.** tokio builds its IO driver's waker from an eventfd. On
   Linux that syscall always exists; on ESP-IDF it is a VFS driver that nothing
   registers for you, so eventfd() returns EACCES and the *runtime itself*
   fails to construct. esp_vfs_eventfd_register() must run before the first
   ts_ffi call.

2. **Explicit thread stacks, split by what the thread touches.** Rust's default
   is 2 MiB; ESP-IDF pthreads default to 2048 bytes. Neither works. Worse, the
   two knobs live in different places: Rust's std::thread calls
   pthread_attr_setstacksize explicitly, which overrides esp_pthread_set_cfg,
   so **Rust owns the size and C++ owns the placement**. Setting a size in
   esp_pthread_set_cfg for a Rust-created thread silently does nothing.

3. **Internal DRAM for anything that touches flash.** A task on a PSRAM stack
   may not be the one that disables the flash cache, and every SPI flash write
   does -- it asserts in esp_task_stack_is_sane_cache_disabled(). This catches
   more threads than expected: tailscale-rs reaches the key state through
   tokio::fs, which dispatches to the blocking pool, so tokio's threads need
   internal stacks too even though they look like pure network workers.

4. **Headroom, measured rather than assumed.** At the moment the runtime is
   built, with WiFi, mDNS, NTP and both web servers up: 110,700 bytes of
   internal DRAM free, largest block 69,620. That is the budget every stack
   above comes out of. 64 KiB fit the worker and then starved the blocking
   pool; 24 KiB fits a worker plus two.

### How these present

Badly, and never as themselves. A Rust panic on a detached thread aborts the
node, so all of them look like a boot loop. The eventfd failure surfaced as
`assert failed: block_locate_free` -- the TLSF allocator tripping inside Rust's
*panic printer* as it allocated a mutex on an already-starved heap, three
layers from the cause and naming none of the parties involved.

Two things made these findable, and both are worth keeping:

- `.expect()` with a message instead of `.unwrap()`. The Os error code
  (EACCES, then ENOMEM) was the entire diagnosis in both cases.
- A heap measurement printed with esp_rom_printf, *not* the deferred logger.
  Deferred logs are drained by the scheduler, so they are lost in exactly the
  situation a crash diagnostic is for.

### Stack sizes

Settled by measurement, not the earlier guesses: tokio's threads get 64 KiB
from PSRAM (one poll of the runtime's actor startup was measured at ~28.5 KB);
the bring-up thread gets 48 KiB of internal DRAM because it does the key-state
flash I/O. None of these are high-water marks yet -- see the status section.

## Known gaps

- **No shutdown primitive in ts_ffi.** Nothing can interrupt a thread parked in
  `ts_tcp_recv` or `ts_tcp_accept`, so a peer that neither sends nor hangs up
  leaves its reader parked until the process exits. The fix is a
  `ts_tcp_shutdown(&stream)` in our ts_ffi fork, issuing the `Command::Close`
  that `TcpStream`'s `Drop` already sends. Worth landing before Xtensa, where a
  parked thread costs a real stack.
- **DERP keepalives are received but not timed.** `ts_derp/src/client.rs` leaves
  resetting the keepalive timer as a TODO, so a dead relay connection is not
  detected. On a flaky link that yields a node that looks joined and is not.
- **Not a low-power configuration.** Enabling WiFi calls
  `esp_wifi_set_ps(WIFI_PS_NONE)` and blocks light sleep, and DERP's 60 s
  keepalive floor rules out duty cycling. Treat a tailnet node as mains or
  solar powered.
- **Config is a flat file, not ModuleConfig.** Deliberate for now: a protobuf
  field means regenerating nanopb sources and carrying a schema change across
  upstream releases.
