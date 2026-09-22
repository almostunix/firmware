# Meshtastic phone API over a tailnet

Serves the existing Meshtastic TCP API (port 4403) on the node's tailnet IP,
via [tailscale-rs](https://github.com/tailscale/tailscale-rs)'s `ts_ffi` C API.
No port forwarding, no public MQTT broker.

`ServerAPI<T>` and `APIServerPort<T, U>` are already generic over their
transport, so `TailscaleServerAPI` contains no protocol logic — a client
reaching the node over Tailscale talks to the same API implementation it would
reach over the LAN. PhoneAPI and StreamAPI are untouched.

## Status: Phase 1 complete, Phase 2 native hardening validated

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
- **`--sendtext` intermittently reports `Broken pipe`, on both transports.**
  Over 5 runs each with the same binary: 5/5 against the stock LAN listener on
  `127.0.0.1:4403`, 3/5 against the tailnet listener. Since the LAN path
  contains none of this shim's code, the shim is not the cause — but it is not
  immune either, and an earlier 0/3 reading on the tailnet was small-sample
  luck. Reads like a client-teardown race that loopback triggers more reliably.
  Noted, not chased.

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

### Sequence the runs to protect the key

An ephemeral key is single-use. Run once with a deliberately invalid key first:
an auth rejection from the control plane proves boot, WiFi association, config
parsing and the join attempt all work, and costs nothing. Only then write the
real key.

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
