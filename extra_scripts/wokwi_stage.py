"""Stage build artifacts under stable names for Wokwi.

Meshtastic renames its outputs with a version+commit suffix
(firmware-heltec-v4-tailscale-2.7.26.abc1234.bin), but wokwi.toml needs a fixed
path and has no globbing. Copy the artifacts to
<build_dir>/wokwi/{firmware.bin,firmware.elf} after every build so the simulator
config can stay static.

firmware.bin is a FULL FLASH IMAGE, not just the factory image. wokwi-cli has no
option for extra flash regions -- it writes one blob at offset 0 -- but the
Tailscale auth key lives in /prefs/tailscale.conf on the LittleFS partition, so
the filesystem has to be merged in or the simulated node boots with no key and
cannot join the tailnet.

The LittleFS offset is read from the partition table inside the factory image
rather than from the CSV, so it cannot drift from the table the device will
actually use. Gaps are filled with 0xFF, which is what erased NOR flash reads as.

If the filesystem image is absent (a plain `pio run` without `-t buildfs`), the
factory image is staged alone and a warning is printed -- the firmware will boot
but will find no config.
"""

import glob
import os
import shutil
import struct

Import("env")

PART_TABLE_OFFSET = 0x8000
PART_ENTRY_MAGIC = b"\xaa\x50"
PART_TYPE_DATA = 0x01
PART_SUBTYPE_SPIFFS = 0x82
ERASED = 0xFF


def _newest(build_dir, pattern):
    matches = [m for m in glob.glob(os.path.join(build_dir, pattern))
               if "%swokwi%s" % (os.sep, os.sep) not in m]
    return max(matches, key=os.path.getmtime) if matches else None


def _find_fs_partition(blob):
    """Locate the spiffs/littlefs data partition in an ESP32 partition table."""
    off = PART_TABLE_OFFSET
    while off + 32 <= len(blob):
        entry = blob[off:off + 32]
        if entry[:2] != PART_ENTRY_MAGIC:
            return None
        ptype, subtype = entry[2], entry[3]
        p_off, p_size = struct.unpack("<II", entry[4:12])
        label = entry[12:28].rstrip(b"\x00").decode("ascii", "replace")
        if ptype == PART_TYPE_DATA and subtype == PART_SUBTYPE_SPIFFS:
            return p_off, p_size, label
        off += 32
    return None


def stage(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    out = os.path.join(build_dir, "wokwi")
    os.makedirs(out, exist_ok=True)

    elf = _newest(build_dir, "*.elf")
    if elf:
        shutil.copy2(elf, os.path.join(out, "firmware.elf"))
        print("wokwi: staged %s -> wokwi/firmware.elf" % os.path.basename(elf))

    factory = _newest(build_dir, "*.factory.bin")
    if not factory:
        print("wokwi: no *.factory.bin in %s -- cannot stage firmware.bin" % build_dir)
        return

    with open(factory, "rb") as fh:
        image = bytearray(fh.read())

    fs_bin = _newest(build_dir, "littlefs-*.bin")
    part = _find_fs_partition(image)

    if fs_bin and part:
        p_off, p_size, label = part
        with open(fs_bin, "rb") as fh:
            fs = fh.read()
        if len(fs) > p_size:
            print("wokwi: filesystem image (%d) exceeds partition '%s' (%d) -- truncating"
                  % (len(fs), label, p_size))
            fs = fs[:p_size]
        if len(image) > p_off:
            print("wokwi: factory image (%d) overruns partition '%s' at 0x%X -- not merging"
                  % (len(image), label, p_off))
        else:
            image.extend(bytes([ERASED]) * (p_off - len(image)))
            image.extend(fs)
            print("wokwi: merged %s into '%s' at 0x%X (%d bytes)"
                  % (os.path.basename(fs_bin), label, p_off, len(fs)))
    elif not fs_bin:
        print("wokwi: WARNING no littlefs-*.bin -- run `pio run -t buildfs` first, "
              "or the node will boot without /prefs/tailscale.conf and cannot join")
    else:
        print("wokwi: WARNING no spiffs partition found in the table -- staging app only")

    dest = os.path.join(out, "firmware.bin")
    with open(dest, "wb") as fh:
        fh.write(image)
    print("wokwi: staged wokwi/firmware.bin (%d bytes, %.1f MB)"
          % (len(image), len(image) / (1024.0 * 1024.0)))


# Hook the same target extra_scripts/esp32_extra.py uses to build the factory
# image ($BUILD_DIR/${PROGNAME}.bin), not the .elf. Hooking the .elf runs this
# before the factory image exists. Because this script is loaded after
# esp32_extra.py, registering on the same target puts our action after theirs.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", stage)
