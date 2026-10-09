"""Link tailscale-rs's ts_ffi into a Tailscale build.

The checkout is the tailscale-rs submodule, or $TAILSCALE_RS_DIR if set.
"""

import os
import subprocess

Import("env", "projenv")


def git(cwd, *args):
    try:
        result = subprocess.run(
            ["git", *args], cwd=cwd, capture_output=True, text=True, check=True
        )
    except (OSError, subprocess.CalledProcessError):
        return ""
    return result.stdout.strip()


project_dir = env.subst("$PROJECT_DIR")
ts_root = os.path.abspath(
    os.environ.get("TAILSCALE_RS_DIR") or os.path.join(project_dir, "tailscale-rs")
)

if not os.path.isfile(os.path.join(ts_root, "Cargo.toml")):
    raise SystemExit(
        "tailscale: no tailscale-rs checkout at %s.\n"
        "Run `git submodule update --init tailscale-rs`, or set TAILSCALE_RS_DIR."
        % ts_root
    )

# Cross builds land in target/<triple>/firmware, host builds in target/firmware.
rust_target = env.GetProjectOption("custom_tailscale_rust_target", "")
cargo_cmd = "cargo build -p ts_ffi --profile firmware"
if rust_target:
    out_dir = os.path.join(ts_root, "target", rust_target, "firmware")
    cargo_cmd = "RUSTUP_TOOLCHAIN=esp %s --target %s -Zbuild-std=std,panic_abort" % (
        cargo_cmd,
        rust_target,
    )
else:
    out_dir = os.path.join(ts_root, "target", "firmware")
archive = os.path.join(out_dir, "libtailscalers.a")

if not os.path.isfile(archive):
    raise SystemExit(
        "tailscale: %s not found. Build it first:\n    cd %s && %s"
        % (archive, ts_root, cargo_cmd)
    )

# The firmware records the tailscale-rs commit it needs as the submodule's gitlink.
pinned = git(project_dir, "rev-parse", ":tailscale-rs")
actual = git(ts_root, "rev-parse", "HEAD")
if pinned and actual and pinned != actual:
    print(
        "tailscale: WARNING: linking tailscale-rs %s, but this firmware pins %s"
        % (actual[:12], pinned[:12])
    )
if git(ts_root, "status", "--porcelain", "--untracked-files=no"):
    print("tailscale: WARNING: %s has uncommitted changes" % ts_root)
print("tailscale: linking %s (tailscale-rs %s)" % (archive, actual[:12] or "unknown"))

# The header comes from the same checkout as the archive; cbindgen writes it there.
ffi_dir = os.path.join(ts_root, "crates", "bindings", "ts_ffi")
if not os.path.isdir(ffi_dir):
    ffi_dir = os.path.join(ts_root, "ts_ffi")  # tailscale-rs before its crates/ layout
projenv.Append(CPPPATH=[ffi_dir])

# LIBS, not build_flags (SCons mangles a .a there) or LINKFLAGS (GNU ld needs it
# after the objects).
env.Append(LIBS=[env.File(archive)])
# Relink when only the Rust side changed.
env.Depends("$BUILD_DIR/${PROGNAME}$PROGSUFFIX", archive)
