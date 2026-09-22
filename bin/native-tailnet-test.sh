#!/bin/sh
# Run the native (macOS) tailnet build against the real control server and
# measure what the simulator can only glimpse -- at zero simulator cost.
#
#   bin/native-tailnet-test.sh <fsdir> <seconds>   [env: TS_TEST_* hooks]
#   INFO_EVERY=<s>  also re-run --info every <s> seconds, to show the node stays
#                   usable across control reconnects (which keep the same IP
#                   and so produce no new "joined" line).
#
# <fsdir> must contain prefs/tailscale.conf. Reports, with timestamps: the join,
# the tailnet listener, when this machine's netmap first shows the node with a
# home relay (it is relay-only, so unreachable until then), a meshtastic --info
# over the tailnet after each (re)connection, and the daemon's open socket
# count every 10s -- a leak across reconnects shows up there.
set -u
fsdir=${1:?fsdir}; dur=${2:?seconds}
bin=$(cd "$(dirname "$0")/.." && pwd)/.pio/build/native-macos-tailscale/meshtasticd
cli=$(cd "$(dirname "$0")/.." && pwd)/.venv-meshtastic/bin/meshtastic
port=${PORT:-4404}
log="$fsdir/daemon.log"

pkill -f "native-macos-tailscale/meshtasticd" 2>/dev/null; sleep 1
: > "$log"
( cd "$fsdir" && exec "$bin" -s -d "$fsdir" -p "$port" ) >> "$log" 2>&1 &
pid=$!
t0=$(date +%s)
el() { printf '%4ds' "$(( $(date +%s) - t0 ))"; }
echo "[$(el)] daemon pid $pid, log $log"

relay_of() {
  /usr/local/bin/tailscale status --json 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
for p in (d.get('Peer') or {}).values():
    if sys.argv[1] in (p.get('TailscaleIPs') or []):
        print(p.get('Relay') or ''); break" "$1"
}

ip=""; relay=""; joins_seen=0; info_after_join=0; next_sock=0; next_info=0
while kill -0 "$pid" 2>/dev/null && [ $(( $(date +%s) - t0 )) -lt "$dur" ]; do
  clean=$(sed 's/\x1b\[[0-9;]*m//g' "$log")
  j=$(printf '%s\n' "$clean" | grep -c "joined tailnet as")
  if [ "$j" -gt "$joins_seen" ]; then
    joins_seen=$j
    ip=$(printf '%s\n' "$clean" | grep -oE "joined tailnet as [^ ]+ \([0-9.]+\)" | tail -1 | grep -oE "[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+")
    echo "[$(el)] joined (#$j) as $ip"; relay=""
  fi
  if [ -n "$ip" ] && [ -z "$relay" ]; then
    relay=$(relay_of "$ip")
    [ -n "$relay" ] && echo "[$(el)] netmap shows home relay '$relay'"
  fi
  if [ -n "$relay" ] && [ -n "${INFO_EVERY:-}" ] && [ "$info_after_join" -ge "$joins_seen" ] && [ $(( $(date +%s) - t0 )) -ge "$next_info" ]; then
    info_after_join=$(( joins_seen - 1 ))   # force a re-run below
  fi
  if [ -n "$relay" ] && [ "$info_after_join" -lt "$joins_seen" ]; then
    next_info=$(( $(date +%s) - t0 + ${INFO_EVERY:-0} ))
    if "$cli" --host "$ip:$port" --info > "$fsdir/info.txt" 2>&1; then
      echo "[$(el)] meshtastic --info over tailnet OK ($(wc -l < "$fsdir/info.txt" | tr -d ' ') lines)"
      info_after_join=$joins_seen
    else
      echo "[$(el)] --info failed: $(tail -1 "$fsdir/info.txt")"
    fi
  fi
  if [ $(( $(date +%s) - t0 )) -ge "$next_sock" ]; then
    echo "[$(el)] open sockets: $(lsof -nP -a -p "$pid" -i 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')"
    next_sock=$(( $(date +%s) - t0 + 10 ))
  fi
  sleep 2
done
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
echo "[$(el)] stopped. reconnect-related log lines:"
sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -E "reconnecting to control|lost control connection|TEST HOOK|no control update|state update stream terminated|dialing control|timed out|deadline reached|probe timed out" | sed 's/^/    /' | cut -c1-170 | head -40
