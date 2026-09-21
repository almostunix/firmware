"""Stage build artifacts under stable names for Wokwi.

Meshtastic renames its outputs with a version+commit suffix
(firmware-heltec-v4-tailscale-2.7.26.abc1234.bin), but wokwi.toml needs a fixed
path and has no globbing. Copy the merged factory image and the ELF to
<build_dir>/wokwi/{firmware.bin,firmware.elf} after every build so the simulator
config can stay static.

The factory image is the right one to hand Wokwi: it already contains the
bootloader, partition table and app at their correct offsets, which is what the
simulator writes to flash at 0.
"""

import glob
import os
import shutil

Import("env")


def stage(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    out = os.path.join(build_dir, "wokwi")
    os.makedirs(out, exist_ok=True)

    pairs = [("*.factory.bin", "firmware.bin"), ("*.elf", "firmware.elf")]
    for pattern, dest in pairs:
        matches = [m for m in glob.glob(os.path.join(build_dir, pattern))
                   if "/wokwi/" not in m]
        if not matches:
            print("wokwi: no %s in %s -- skipping %s" % (pattern, build_dir, dest))
            continue
        newest = max(matches, key=os.path.getmtime)
        shutil.copy2(newest, os.path.join(out, dest))
        print("wokwi: staged %s -> wokwi/%s (%d bytes)"
              % (os.path.basename(newest), dest, os.path.getsize(newest)))


# Hook the same target extra_scripts/esp32_extra.py uses to build the factory
# image ($BUILD_DIR/${PROGNAME}.bin), not the .elf. Hooking the .elf runs this
# before the factory image exists. Because this script is loaded after
# esp32_extra.py, registering on the same target puts our action after theirs.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", stage)
