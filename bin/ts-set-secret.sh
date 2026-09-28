#!/bin/sh
# Write a Tailscale or Wokwi secret to disk without it reaching shell history, the process table
# or a chat transcript. Validates the shape first, and reports a verdict, never the value.
#
#   bin/ts-set-secret.sh wokwi                  -> .wokwi-env  (export WOKWI_CLI_TOKEN=...)
#   bin/ts-set-secret.sh tailscale              -> data/prefs/tailscale.conf, simulator node
#   bin/ts-set-secret.sh tailscale-hw [name]    -> the same file, for a real board: hostname
#                                                  [name] (default meshtastic-heltec-v4) and a
#                                                  marker bin/wokwi-run.sh refuses to upload
#
# Run it from the firmware root.
set -eu

usage() { echo "usage: $0 {wokwi|tailscale|tailscale-hw [hostname]}" >&2; exit 2; }
[ $# -ge 1 ] || usage

hostname=meshtastic-wokwi
if [ "$1" = tailscale-hw ]; then
  [ $# -le 2 ] || usage
  hostname=${2:-meshtastic-heltec-v4}
  case "$hostname" in
    ""|-*|*[!a-z0-9-]*) echo "refused: hostname must be lowercase letters, digits and '-'" >&2; exit 2 ;;
  esac
  [ ${#hostname} -le 63 ] || { echo "refused: hostname longer than 63 characters" >&2; exit 2; }
else
  [ $# -eq 1 ] || usage
fi

umask 077

case "$1" in
  wokwi)     dest=".wokwi-env"; label="Wokwi CI token" ;;
  tailscale|tailscale-hw) dest="data/prefs/tailscale.conf"; label="Tailscale auth key" ;;
  *)         usage ;;
esac

printf '%s: ' "$label" >&2
trap 'stty echo 2>/dev/null' EXIT INT TERM  # never leave the terminal silent after Ctrl-C
stty -echo 2>/dev/null || true
IFS= read -r VALUE || true
stty echo 2>/dev/null || true
printf '\n' >&2

# A paste often carries a trailing newline or space; no key contains whitespace.
VALUE=$(printf '%s' "$VALUE" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')

len=$(printf '%s' "$VALUE" | wc -c | tr -d ' ')

if [ "$len" -eq 0 ]; then
  echo "refused: empty input, $dest not written" >&2
  exit 1
fi

# Shell metacharacters mean this is not a token; .wokwi-env is sourced, so they would be code.
case "$VALUE" in
  *[!A-Za-z0-9._-]*)
    echo "refused: contains characters outside [A-Za-z0-9._-] (length $len)." >&2
    echo "         that is prose or a command line, not a token. $dest not written." >&2
    exit 1 ;;
esac

if [ "$1" != wokwi ]; then
  case "$VALUE" in
    tskey-*tskey-*)
       echo "refused: that is more than one key pasted together (length $len). $dest not written." >&2
       exit 1 ;;
    tskey-*) ;;
    *) echo "refused: a Tailscale auth key starts with 'tskey-' (length $len)." >&2
       echo "         did you paste the Wokwi token here? $dest not written." >&2
       exit 1 ;;
  esac
  # printf is a shell builtin, so the key reaches grep through the pipe, never argv.
  if ! printf '%s' "$VALUE" | grep -Eq '^tskey-[a-z]+-[A-Za-z0-9]+-[A-Za-z0-9]+$'; then
    echo "refused: not shaped like one auth key, tskey-<kind>-<id>-<secret> (length $len). $dest not written." >&2
    exit 1
  fi
else
  case "$VALUE" in
    tskey-*) echo "refused: that is a Tailscale key, not a Wokwi token. $dest not written." >&2
             exit 1 ;;
  esac
fi

mkdir -p "$(dirname "$dest")"

if [ "$1" = wokwi ]; then
  # Single-quoted, so sourcing the file can never run anything.
  printf "export WOKWI_CLI_TOKEN='%s'\n" "$VALUE" > "$dest"
else
  { [ "$1" = tailscale-hw ] && printf '# target=hardware -- bin/wokwi-run.sh refuses images containing this line\n'
    printf 'authkey=%s\n' "$VALUE"
    printf 'hostname=%s\n' "$hostname"; } > "$dest"
fi

chmod 600 "$dest"
VALUE=

echo "wrote $dest (mode $(stat -f %Lp "$dest" 2>/dev/null || stat -c %a "$dest"), value length $len)" >&2
