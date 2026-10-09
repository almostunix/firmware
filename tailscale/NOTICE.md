# Notice: the unofficial `tailscale-heltec-v4` branch

The `tailscale-heltec-v4` branch of this repository adds Tailscale networking to the Meshtastic
firmware. It is an independent, unofficial branch. **This branch is not affiliated with or endorsed by
the Meshtastic project, and is not affiliated with, sponsored by, or endorsed by Meshtastic LLC or
Tailscale Inc.**

Meshtastic® is a registered trademark of Meshtastic LLC. The Meshtastic logo trademark is the trademark
of Meshtastic LLC; the logo in [README.md](README.md) is upstream's and links to
<https://meshtastic.org>. Tailscale® is a registered trademark of Tailscale Inc. Both names are used
here only to describe what the software works with, not as the name of this branch or of any product.

## Credits

- **Meshtastic firmware**, by Meshtastic LLC and contributors: <https://github.com/meshtastic/firmware>.
  This branch is based on release `v2.7.26.54e0d8d`, and its full history is kept, so every upstream
  change remains credited to its author.
- **tailscale-rs**, by Tailscale Inc and contributors: <https://github.com/tailscale/tailscale-rs>.
  The firmware links a fork of it, based on release `v0.6.1`:
  <https://github.com/almostunix/tailscale-rs>, branch `heltec-v4`, included here as the `tailscale-rs`
  submodule. Tailscale's own history is kept there too.
- The changes on these branches were written with the help of Claude (Anthropic), as the
  `Co-Authored-By` lines on the commits record. They were reviewed and tested with unit tests and on a
  Heltec V4 over a real tailnet.

## Licences

- **Meshtastic firmware:** GNU General Public License v3.0 (see [LICENSE](../LICENSE)). The changes on this
  branch are released under the same licence.
- **tailscale-rs:** BSD 3-Clause License, plus Tailscale's additional patent grant (`LICENSE` and
  `PATENTS` in the tailscale-rs repository). The changes on its `heltec-v4` branch are released under the
  same terms. BSD 3-Clause is compatible with the GPL, so the combined firmware can be distributed under
  GPL-3.0, with the tailscale-rs notice kept.
- **Other components in a built firmware image** keep their own licences, including ESP-IDF and mbedTLS
  (Apache-2.0), the Arduino-ESP32 core (LGPL-2.1), NimBLE-Arduino (Apache-2.0), and the Rust crates
  tailscale-rs depends on (mostly MIT or Apache-2.0; list them with `cargo tree` or `cargo about` in the
  tailscale-rs repository).

If you distribute firmware binaries built from this branch, the GPL requires you to offer the
corresponding source: this branch at the commit you built, the tailscale-rs commit its submodule pins,
and the build steps in [QUICKSTART.md](QUICKSTART.md).

## What was changed (2026-09-21 to 2026-10-09)

**Added to Meshtastic:**
- Tailscale transport: `src/mesh/api/Tailscale*` (server API, socket shim, mbedTLS verifier)
- mbedTLS allocations in PSRAM: `src/platform/esp32/MbedtlsPsram.cpp`
- Path checks: `src/FSPath.h`
- Tests: `test/test_fspath`, `test/test_tailscale_shim`
- The `tailscale-rs` submodule
- Scripts: `bin/` (Tailscale, Wokwi and benchmark scripts), `extra_scripts/`
- `tailscale/`: the docs (this file among them), `tailscale.conf.example`, and the Wokwi simulator
  setup in `tailscale/wokwi/`

**Modified in Meshtastic:** `.gitignore`, `.gitmodules`, `README.md`, `src/Power.cpp` and `src/power.h` (battery ADC on
the ESP-IDF 5 driver), `src/main.cpp`, `src/mesh/InterfacesTemplates.cpp`, `src/mesh/PhoneAPI.cpp`,
`src/mesh/RadioInterface.cpp` (simulator only), `src/mesh/http/ContentHandler.cpp`,
`src/mesh/http/WebServer.cpp`, `src/mesh/wifi/WiFiAPClient.cpp`, `src/platform/esp32/main-esp32.cpp`
(Bluetooth memory, watchdog), `src/xmodem.cpp`, `variants/esp32s3/heltec_v4/platformio.ini`,
`variants/native/portduino/platformio.ini`

**Changed in tailscale-rs:** an ESP-IDF (ESP32-S3) port, and fixes in `ts_control`, `ts_derp`,
`ts_ffi`, `ts_netcheck`, `ts_netmon`, `ts_netstack_smoltcp*`, `ts_runtime`, `ts_tls_util` and
`ts_tunnel`. The commit messages on the `heltec-v4` branch describe each change.
