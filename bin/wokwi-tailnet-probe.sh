#!/bin/sh
# Connect to the simulated node over the tailnet; run alongside bin/wokwi-run.sh tailnet.
# tailscale-rs is relay-only, so this waits for the listener, then for this machine's netmap to show the
# node's home relay, then retries `meshtastic --info`. PROBE_STOP_ON_SUCCESS=1 ends the run once it works.
set -u
deadline=$(( $(date +%s) + ${PROBE_WAIT_S:-600} ))
t0=$(date +%s)
el() { echo "$(( $(date +%s) - t0 ))s"; }

log=""
while [ -z "$log" ] && [ "$(date +%s)" -lt "$deadline" ]; do
  log=$(ls -t .pio/wokwi-logs/tailnet-*.log 2>/dev/null | head -1); [ -n "$log" ] || sleep 2
done
[ -n "$log" ] || { echo "probe: no tailnet run log appeared" >&2; exit 2; }
echo "probe [$(el)]: watching $log" >&2

addr=""
while [ -z "$addr" ] && [ "$(date +%s)" -lt "$deadline" ]; do
  addr=$(LC_ALL=C sed 's/\x1b\[[0-9;]*m//g' "$log" | LC_ALL=C grep -aoE 'API server listening on [0-9.]+:[0-9]+' | head -1 | awk '{print $NF}')
  [ -n "$addr" ] || sleep 2
done
[ -n "$addr" ] || { echo "probe [$(el)]: node never reported a tailnet listener" >&2; exit 1; }
ip=${addr%:*}; port=${addr##*:}
echo "probe [$(el)]: node listening on $ip:$port" >&2

peer_field() {  # peer_field <ip> <field>: this machine's view of the peer
  /usr/local/bin/tailscale status --json 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
for p in (d.get('Peer') or {}).values():
    if sys.argv[1] in (p.get('TailscaleIPs') or []):
        print(p.get(sys.argv[2]) or ''); break
" "$1" "$2"
}

sim_alive() { pgrep -f "serial-log-file $log" >/dev/null; }

relay=""
while [ -z "$relay" ] && sim_alive && [ "$(date +%s)" -lt "$deadline" ]; do
  relay=$(peer_field "$ip" Relay); [ -n "$relay" ] || sleep 3
done
if [ -z "$relay" ]; then
  echo "probe [$(el)]: node NEVER advertised a home relay -- unreachable by design" >&2
  exit 1
fi
echo "probe [$(el)]: node advertised home relay '$relay'" >&2

cli=$(command -v meshtastic || true)
[ -z "$cli" ] && [ -x .venv-meshtastic/bin/meshtastic ] && cli=.venv-meshtastic/bin/meshtastic

attempt=0
while sim_alive && [ "$(date +%s)" -lt "$deadline" ]; do
  attempt=$((attempt + 1))
  if nc -z -G 10 "$ip" "$port" 2>/dev/null; then
    echo "probe [$(el)]: TCP connect OK on attempt $attempt" >&2
    [ -n "$cli" ] || { echo "probe: meshtastic CLI not installed -- TCP only" >&2; exit 0; }
    if "$cli" --host "$ip:$port" --info > "$log.info.txt" 2>&1; then
      echo "probe [$(el)]: meshtastic --info OK ($(wc -l < "$log.info.txt" | tr -d ' ') lines -> $log.info.txt)" >&2
      if [ "${PROBE_STOP_ON_SUCCESS:-0}" = 1 ]; then
        echo "probe: success -- ending the simulation to save minutes" >&2
        pkill -f "serial-log-file $log" 2>/dev/null
      fi
      exit 0
    fi
    echo "probe [$(el)]: --info failed: $(tail -1 "$log.info.txt")" >&2
  else
    echo "probe [$(el)]: TCP connect attempt $attempt timed out" >&2
  fi
  sleep 3
done
echo "probe [$(el)]: gave up (simulation ended or deadline)" >&2
exit 1
