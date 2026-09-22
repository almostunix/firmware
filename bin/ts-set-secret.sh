#!/bin/sh
# Write a Tailscale/Wokwi secret to disk without it touching the shell history,
# the process table, or an AI transcript.
#
# Both secrets previously went in via a bare `read -s` one-liner. That failed
# twice in one sitting: a clipboard route pasted the command text itself into
# the token file, and the Wokwi token was typed at the Tailscale prompt. Neither
# was caught until the files were inspected, because nothing validated them and
# an invisible prompt gives no feedback.
#
# So this validates before writing, and reports only a verdict -- never a value.
#
#   bin/ts-set-secret.sh wokwi       -> .wokwi-env  (export WOKWI_CLI_TOKEN=...)
#   bin/ts-set-secret.sh tailscale   -> data/prefs/tailscale.conf
#
# Run it from the firmware root.
set -eu

usage() { echo "usage: $0 {wokwi|tailscale}" >&2; exit 2; }
[ $# -eq 1 ] || usage

umask 077

case "$1" in
  wokwi)     dest=".wokwi-env"; label="Wokwi CI token" ;;
  tailscale) dest="data/prefs/tailscale.conf"; label="Tailscale auth key" ;;
  *)         usage ;;
esac

printf '%s: ' "$label" >&2
stty -echo 2>/dev/null || true
IFS= read -r VALUE || true
stty echo 2>/dev/null || true
printf '\n' >&2

len=$(printf '%s' "$VALUE" | wc -c | tr -d ' ')

if [ "$len" -eq 0 ]; then
  echo "refused: empty input, $dest not written" >&2
  exit 1
fi

# Shell metacharacters mean this is not a token. It matters most for
# .wokwi-env, which gets sourced -- a stray ; or $( ) there is code execution.
case "$VALUE" in
  *[!A-Za-z0-9._-]*)
    echo "refused: contains characters outside [A-Za-z0-9._-] (length $len)." >&2
    echo "         that is prose or a command line, not a token. $dest not written." >&2
    exit 1 ;;
esac

if [ "$1" = tailscale ]; then
  case "$VALUE" in
    tskey-*) ;;
    *) echo "refused: a Tailscale auth key starts with 'tskey-' (length $len)." >&2
       echo "         did you paste the Wokwi token here? $dest not written." >&2
       exit 1 ;;
  esac
else
  case "$VALUE" in
    tskey-*) echo "refused: that is a Tailscale key, not a Wokwi token. $dest not written." >&2
             exit 1 ;;
  esac
fi

mkdir -p "$(dirname "$dest")"

if [ "$1" = wokwi ]; then
  # Single-quote the value so that sourcing this file can never execute
  # anything, even if the validation above is ever loosened.
  printf "export WOKWI_CLI_TOKEN='%s'\n" "$VALUE" > "$dest"
else
  { printf 'authkey=%s\n' "$VALUE"
    printf 'hostname=meshtastic-wokwi\n'; } > "$dest"
fi

chmod 600 "$dest"
VALUE=

echo "wrote $dest (mode $(stat -f %Lp "$dest" 2>/dev/null || stat -c %a "$dest"), value length $len)" >&2
