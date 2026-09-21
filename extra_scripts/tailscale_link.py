"""Link the tailscale-rs static archive into the native Tailscale build.

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
archive = os.path.normpath(os.path.join(ts_root, "target", "release", "libtailscalers.a"))

if not os.path.isfile(archive):
    raise SystemExit(
        "tailscale: %s not found.\n"
        "Build the Rust side first:\n"
        "    cd %s && cargo build -p ts_ffi --release\n"
        "Or point TAILSCALE_RS_DIR at the tailscale-rs checkout." % (archive, os.path.normpath(ts_root))
    )

env.Append(LINKFLAGS=[archive])

# LINKFLAGS is an opaque value to SCons, not a file dependency, so changing the
# archive does NOT invalidate the link by itself. Without this the build happily
# reports SUCCESS while linking a stale libtailscalers.a -- which bit us once:
# a Rust rebuild that cut 4.9MB off the archive produced a byte-identical
# firmware until the binary was deleted by hand. Declare the dependency so a
# `cargo build` is enough to force a relink.
env.Depends("$BUILD_DIR/${PROGNAME}$PROGSUFFIX", archive)
