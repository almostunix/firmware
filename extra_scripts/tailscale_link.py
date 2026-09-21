"""Link the tailscale-rs static archive into a Tailscale-enabled build.

The archive cannot simply be listed in `build_flags`: PlatformIO runs those
through SCons' ParseFlags, which classifies anything ending in `.a` as a LIBS
entry, drops the suffix, and then resolves what is left relative to the
*platform* directory rather than the project. That is why a plain
`../tailscale-rs-0.6.1/target/release/libtailscalers.a` failed the link looking
for `~/.platformio/platforms/native/.../libtailscalers`.

Appending to LINKFLAGS instead hands the archive to the linker verbatim, as a
positional input, which is what we want -- and keeps it a *static* link rather
than letting `-ltailscalers` silently prefer the .dylib cargo builds alongside
it.
"""

import os

Import("env")

project_dir = env.subst("$PROJECT_DIR")
ts_root = os.environ.get("TAILSCALE_RS_DIR") or os.path.join(project_dir, "..", "tailscale-rs-0.6.1")

# Cargo puts a cross-compiled build under target/<triple>/, and a host build
# straight under target/. Pick by the Rust target this PlatformIO env needs, so
# an ESP32 env can never silently link the macOS archive (which would fail with
# a wall of unhelpful architecture errors rather than saying what was wrong).
rust_target = env.GetProjectOption("custom_tailscale_rust_target", "")
if rust_target:
    out_dir = os.path.join(ts_root, "target", rust_target, "release")
    build_hint = "--target %s -Zbuild-std=std,panic_abort" % rust_target
    toolchain_hint = "RUSTUP_TOOLCHAIN=esp "
else:
    out_dir = os.path.join(ts_root, "target", "release")
    build_hint = ""
    toolchain_hint = ""

archive = os.path.normpath(os.path.join(out_dir, "libtailscalers.a"))

if not os.path.isfile(archive):
    raise SystemExit(
        "tailscale: %s not found.\n"
        "Build the Rust side first:\n"
        "    cd %s && %scargo build -p ts_ffi --release %s\n"
        "Or point TAILSCALE_RS_DIR at the tailscale-rs checkout."
        % (archive, os.path.normpath(ts_root), toolchain_hint, build_hint)
    )

print("tailscale: linking %s" % archive)

# Goes in LIBS, not LINKFLAGS. SCons emits LINKFLAGS *before* the object files,
# and GNU ld resolves a static archive only against references it has already
# seen -- so an archive placed first contributes nothing and every ts_* symbol
# comes back undefined. (Apple's ld is order-insensitive here, which is why the
# native build worked with LINKFLAGS and hid this.) LIBS lands after the
# objects, where GNU ld can actually use it.
env.Append(LIBS=[env.File(archive)])

# LIBS entries are tracked by SCons, but be explicit anyway: this dependency is
# what forces a relink when only the Rust side changed. Without it the build
# once reported SUCCESS while linking a stale libtailscalers.a -- a Rust rebuild
# that cut 4.9MB off the archive produced a byte-identical firmware.
env.Depends("$BUILD_DIR/${PROGNAME}$PROGSUFFIX", archive)
