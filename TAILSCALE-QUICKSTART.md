# Tailscale branch: quick start

This unofficial branch of the Meshtastic firmware adds [Tailscale](https://tailscale.com), using
[tailscale-rs](https://github.com/tailscale/tailscale-rs). The node joins your tailnet over WiFi and
serves the normal Meshtastic phone API on its tailnet address, port 4403. You can reach it from anywhere
on your tailnet with no port forwarding. The LAN API, LoRa and (with WiFi off) Bluetooth work as usual.

It is not affiliated with or endorsed by the Meshtastic project, Meshtastic LLC or Tailscale Inc. See
[NOTICE-TAILSCALE.md](NOTICE-TAILSCALE.md) for credits and licences, and [TAILSCALE.md](TAILSCALE.md)
for the engineering notes.

**Tested on:** Heltec WiFi LoRa 32 V4 (ESP32-S3, 16 MB flash, 2 MB PSRAM) only.
**Status:** tailscale-rs is beta software. All tailnet traffic is relayed through Tailscale's DERP
servers, because tailscale-rs does not do direct connections yet.

## What you need

- A Heltec V4, a USB data cable, and a 915/868 MHz antenna fitted before power-on.
- A Tailscale account.
- macOS or Linux with [PlatformIO](https://platformio.org/install/cli) and
  [espup](https://github.com/esp-rs/espup) (the Rust toolchain for the ESP32).
- Chrome or Edge, for [flasher.meshtastic.org](https://flasher.meshtastic.org).

## 1. Get the source

tailscale-rs comes along as a git submodule, pinned to the commit this firmware was tested with:

```bash
git clone --recurse-submodules -b tailscale-heltec-v4 https://github.com/almostunix/firmware firmware
cd firmware
```

If you cloned without `--recurse-submodules`, run `git submodule update --init` in the `firmware` folder.

## 2. Add your Tailscale key

1. In the Tailscale admin console, go to **Settings → Keys → Generate auth key**.
   - **Reusable:** off. One key per node.
   - **Ephemeral:** off, or the node disappears from your tailnet whenever it goes offline.
   - **Pre-approved:** on, if your tailnet requires device approval.
   - **Tags:** optional.
2. From the `firmware` folder, write the key and choose the node's tailnet name:

   ```bash
   bin/ts-set-secret.sh tailscale-hw my-meshtastic-node
   ```

   Paste the key at the hidden prompt. It is checked for the right shape and written to
   `data/prefs/tailscale.conf`, which git ignores. Never commit that file or paste the key anywhere else.
   The `tailscale-hw` mode also marks the key as a hardware key, so the simulator scripts refuse to use it.

The key is only used for the node's first join. After that the node keeps its own identity in its
filesystem, so a spent single-use key is fine. Use a new key only for a node that has been fully erased.

## 3. Build

```bash
# The Rust library, cross-compiled for the ESP32-S3 (several minutes the first time)
. ~/export-esp.sh
(cd tailscale-rs && RUSTUP_TOOLCHAIN=esp cargo build -p ts_ffi --profile firmware \
  --target xtensa-esp32s3-espidf -Zbuild-std=std,panic_abort)

# The firmware, and a filesystem image that carries your key
pio run -e heltec-v4-tailscale
pio run -e heltec-v4-tailscale -t buildfs
```

Use `heltec-v4-tailscale-nobt` instead of `heltec-v4-tailscale` for a build without Bluetooth.

## 4. Make the flasher zips

Download Meshtastic's OTA loader for the ESP32-S3, then package the build:

```bash
curl -fLO https://release.meshtastic.org/2.7.26.54e0d8d/mt-esp32s3-ota.bin
bin/make-flasher-zip.sh --ota mt-esp32s3-ota.bin -l my-node
```

This writes two files to `release-zips/`:
- **`my-node-first-install.zip`** contains your key. Keep it private, and delete it once the node has
  joined.
- **`my-node-update.zip`** is the app alone, with no key. Use it for every later update.

## 5. First install

At [flasher.meshtastic.org](https://flasher.meshtastic.org):

1. **Device:** Heltec V4.
2. Click **upload** (the folder icon) and choose `my-node-first-install.zip`.
3. Leave **Install MUI** and **Bundle Web UI** unticked. Tick **Full Erase and Install**.
4. Click **Erase Flash and Install** and pick the board's port.

If the flasher can't connect, unplug the board, hold **BOOT/PRG**, plug it back in, and release.

## 6. WiFi and region

At [client.meshtastic.org](https://client.meshtastic.org), over USB:

- **Config → Network:** turn WiFi on and enter your SSID and password.
- **Config → LoRa:** set your region.

Save. The node reboots, joins your WiFi, then joins your tailnet within about 30 seconds. Close the
browser tab afterwards; a USB client holding the port suppresses the serial log.

## 7. Connect over your tailnet

The node appears in the Tailscale admin console under the name you chose. Then:

- **Admin console:** open the node and choose **Disable key expiry**, or it drops off the tailnet after
  180 days.
- **Phone:** with the Tailscale app connected, add the node in the Meshtastic app by its tailnet IP,
  port 4403.
- **Computer:** `meshtastic --host <tailnet-ip> --info`

## Updating later

Pull, update the submodule (`git submodule update`), rebuild (steps 3 and 4), then at the flasher upload `my-node-update.zip` and click **Update**. Do not
tick Full Erase: an update keeps the node's settings and its tailnet identity, while a full erase needs a
new key.

## Things to know

- **Bluetooth only runs while WiFi is off.** That is stock Meshtastic behaviour. With WiFi on, use the
  tailnet, the LAN address, or USB.
- **Secrets stay on the node.** The Meshtastic API and the web server refuse to serve, list or overwrite
  `/prefs/tailscale.conf` and the node's Tailscale keys.
- **Unit tests:** see "Building and testing" in [TAILSCALE.md](TAILSCALE.md).
