# Meshtastic phone API over a tailnet

Serves the existing Meshtastic TCP API (port 4403) on the node's tailnet IP,
via [tailscale-rs](https://github.com/tailscale/tailscale-rs)'s `ts_ffi` C API.
No port forwarding, no public MQTT broker.

`ServerAPI<T>` and `APIServerPort<T, U>` are already generic over their
transport, so `TailscaleServerAPI` contains no protocol logic — a client
reaching the node over Tailscale talks to the same API implementation it would
reach over the LAN. PhoneAPI and StreamAPI are untouched.

## Status: Phase 1 complete (native macOS ARM64)

Verified end to end on 2026-09-21: `meshtastic --host 100.x.y.z:4403 --info`,
`--nodes` and `--sendtext` all work against `meshtasticd -s`, over DERP, with
each connection accepted and closed cleanly.

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
| \+ rustls-rustcrypto | 17,087,368 | 7,600,632 |
| \+ `opt-level = "z"`, `codegen-units = 1`, `panic = "abort"` | 13,014,712 | **4,242,772** |

So Phase 2 has taken the added code from **+6.6 MB down to +3.0 MB**, a 47%
cut in `__text`. Against `default_16MB.csv`'s 6.25 MB app partition — which
also has to hold Meshtastic's own ~1 MB — that now fits with room to spare,
where the Phase 1 figure did not.

The attribution is worth recording, because it is not what was expected:

- **the cargo profile is the dominant lever** (-3.36 MB, 44%). Note the profile
  was *already* `lto = true, opt-level = "s", strip = true` — the win came from
  going further to `"z"` plus one codegen unit and `panic = "abort"`, not from
  turning on optimisation that was missing.
- **the aws-lc-rs -> rustls-rustcrypto swap is nearly irrelevant to size**
  (-372 KB, 4.7%). It is required because aws-lc-rs cannot build for Xtensa,
  but it should not be sold as a size measure. It also drags in a second
  `rustls-webpki` (0.102.8 alongside 0.103.15), which partly cancels the gain.

Still untouched, so still available: dropping `ts_netcheck`, `ts_netmon` and
`ts_transport_tun`; `current_thread` tokio; and de-duplicating webpki.

Measure with a forced relink, or trust nothing — see the note in
`extra_scripts/tailscale_link.py`.

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
