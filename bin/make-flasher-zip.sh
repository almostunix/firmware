#!/bin/sh
# Package a PlatformIO build for the "upload" (folder) button at https://flasher.meshtastic.org.
#
#   bin/make-flasher-zip.sh --ota mt-esp32s3-ota.bin [-e ENV] [-l LABEL] [-o OUTDIR]
#
#   -e ENV     heltec-v4-tailscale (default, with Bluetooth) or heltec-v4-tailscale-nobt
#   -l LABEL   name prefix for the zips (default: the env name)
#   -o OUTDIR  where to write them (default: release-zips)
#   --ota F    Meshtastic's OTA loader for the ESP32-S3, required for a first install:
#              https://release.meshtastic.org/2.7.26.54e0d8d/mt-esp32s3-ota.bin
#
# Writes:
#   LABEL-first-install.zip  full image + OTA loader + filesystem WITH YOUR TAILSCALE KEY.
#                            Flash with "Full Erase and Install". Keep it private; delete it after.
#   LABEL-update.zip         the app only, no key. Flash with "Update": keeps settings and tailnet identity.
#
# Build first:  pio run -e ENV  and  pio run -e ENV -t buildfs  (after bin/ts-set-secret.sh tailscale-hw).
# The flasher picks files by name, so the names inside the zips matter; the zip names do not.
set -eu

env=heltec-v4-tailscale
label=""
out=release-zips
ota=""
while [ $# -gt 0 ]; do
  case "$1" in
    -e) env=$2; shift 2 ;;
    -l) label=$2; shift 2 ;;
    -o) out=$2; shift 2 ;;
    --ota) ota=$2; shift 2 ;;
    -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done
label=${label:-$env}
build=.pio/build/$env

[ -d "$build" ] || { echo "no build in $build -- run: pio run -e $env" >&2; exit 1; }

# The app image is firmware-ENV-VERSION.bin; the same stem names the factory and filesystem images.
app=$(ls "$build"/firmware-"$env"-*.bin 2>/dev/null | grep -v '\.factory\.bin$' | head -1 || true)
[ -n "$app" ] || { echo "no firmware image in $build -- run: pio run -e $env" >&2; exit 1; }
stem=$(basename "$app" .bin)               # firmware-ENV-VERSION
version=${stem#firmware-"$env"-}
factory=$build/$stem.factory.bin
fs=$build/littlefs-$env-$version.bin

case "$stem" in
  firmware-heltec-v4-*) ;;
  *) echo "refused: the flasher only recognises firmware-heltec-v4-* images, and this is $stem" >&2; exit 1 ;;
esac

mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Update zip: the app alone, named ...-update.bin so the flasher writes it at 0x10000.
cp "$app" "$tmp/$stem-update.bin"
(cd "$tmp" && zip -q -X "update.zip" "$stem-update.bin")
mv "$tmp/update.zip" "$out/$label-update.zip"
echo "wrote $out/$label-update.zip (flash with Update)"

# First-install zip: only when the filesystem image carries a hardware key.
if [ -z "$ota" ]; then
  echo "skipped the first-install zip: pass --ota with Meshtastic's mt-esp32s3-ota.bin (URL in --help)" >&2
  exit 0
fi
[ -f "$ota" ] || { echo "no OTA loader at $ota" >&2; exit 1; }
[ -f "$factory" ] || { echo "no factory image $factory -- run: pio run -e $env" >&2; exit 1; }
[ -f "$fs" ] || { echo "no filesystem image $fs -- run: pio run -e $env -t buildfs" >&2; exit 1; }
if ! LC_ALL=C grep -aq 'authkey=tskey-' "$fs"; then
  echo "refused: $fs has no Tailscale key. Run bin/ts-set-secret.sh tailscale-hw, then pio run -e $env -t buildfs" >&2
  exit 1
fi
if ! LC_ALL=C grep -aq 'target=hardware' "$fs"; then
  echo "refused: the key in $fs was written for the simulator. Use bin/ts-set-secret.sh tailscale-hw" >&2
  exit 1
fi
if [ data/prefs/tailscale.conf -nt "$fs" ]; then
  echo "warning: data/prefs/tailscale.conf is newer than $fs; rebuild with -t buildfs if you changed the key" >&2
fi

umask 077
cp "$factory" "$tmp/$stem.bin"
cp "$app" "$tmp/$stem-update.bin"
cp "$ota" "$tmp/bleota-s3.bin"
cp "$fs" "$tmp/littlefs-$env-$version.bin"
(cd "$tmp" && zip -q -X "first.zip" "$stem.bin" "$stem-update.bin" bleota-s3.bin "littlefs-$env-$version.bin")
mv "$tmp/first.zip" "$out/$label-first-install.zip"
chmod 600 "$out/$label-first-install.zip"
echo "wrote $out/$label-first-install.zip (flash with Full Erase and Install)"
echo "  it contains your Tailscale auth key: keep it private and delete it once the node has joined"
