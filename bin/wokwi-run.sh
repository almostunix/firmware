#!/bin/sh
# Run a Wokwi scenario so that it spends only the CI minutes it needs.
#
#   bin/wokwi-run.sh dry       dummy key: runtime, join attempt, battery cutoff
#   bin/wokwi-run.sh tailnet   real key:  join, API listening, battery cutoff
#   bin/wokwi-run.sh diag-*    dummy key: a diagnostic scenario; nothing is
#                              aborted on, since observing is the point
#
# Minutes are the scarce resource (free plan: 50/month, 5 min per run). A
# scenario ends the simulation as soon as its last step passes, and
# --fail-text ends it as soon as the node resets: every crash seen so far --
# panic, assert, abort -- finishes with "Rebooting...".
#
# The staged image is checked before anything is uploaded, because the wrong
# key in the wrong scenario is expensive in both directions: a real key in a
# dry run spends a single-use key on a test that does not need it, and the
# dummy in a tailnet run wastes minutes on a join that cannot succeed.
set -u

scenario="${1:-}"
case "$scenario" in
  dry|tailnet|diag-*) ;;
  *) echo "usage: $0 {dry|tailnet|diag-<name>}" >&2; exit 2 ;;
esac
[ -f "wokwi-scenarios/$scenario.yaml" ] || { echo "no scenario wokwi-scenarios/$scenario.yaml" >&2; exit 2; }

# Diagnostics run on the dummy key, same as a dry run.
key_mode=$scenario
case "$scenario" in diag-*) key_mode=dry ;; esac

file="wokwi-scenarios/$scenario.yaml"
image=".pio/build/heltec-v4-tailscale-wokwi/wokwi/firmware.bin"

[ -f .wokwi-env ] || { echo "no .wokwi-env -- run bin/ts-set-secret.sh wokwi" >&2; exit 2; }
[ -f "$image" ]   || { echo "no staged image -- run: pio run -e heltec-v4-tailscale-wokwi -t buildfs && pio run -e heltec-v4-tailscale-wokwi" >&2; exit 2; }

if LC_ALL=C grep -aq "PIPELINEPROBE" "$image"; then has_dummy=1; else has_dummy=0; fi
if LC_ALL=C grep -aq "authkey=tskey-" "$image"; then has_key=1; else has_key=0; fi

# A real board's key must never reach Wokwi's servers (bin/ts-set-secret.sh tailscale-hw).
if LC_ALL=C grep -aq "target=hardware" "$image"; then
  echo "refused: the staged image carries a hardware board's Tailscale key. Write a" >&2
  echo "         simulator key (bin/ts-set-secret.sh tailscale) and rebuild first." >&2
  exit 1
fi

if [ "$key_mode" = dry ] && [ "$has_dummy" -ne 1 ]; then
  echo "refused: the staged image does not carry the dummy key. A dry run with a" >&2
  echo "         real single-use key would spend it. Rebuild the filesystem with the" >&2
  echo "         dummy in data/prefs/tailscale.conf first." >&2
  exit 1
fi
if [ "$key_mode" = tailnet ] && { [ "$has_dummy" -eq 1 ] || [ "$has_key" -ne 1 ]; }; then
  echo "refused: the staged image does not carry a real key. Run" >&2
  echo "         bin/ts-set-secret.sh tailscale, then rebuild with -t buildfs and pio run." >&2
  exit 1
fi

mkdir -p .pio/wokwi-logs
log=".pio/wokwi-logs/$scenario-$(date +%Y%m%d-%H%M%S).log"

# Two independent limits, because both of the obvious ones turned out not to
# work (each cost real minutes to discover):
#
#  1. Crash detection. --fail-text "Rebooting..." was assumed to catch every
#     crash. It does not: a Rust double panic ("thread panicked while
#     processing panic. aborting.") resets the chip without printing it, and 14
#     of those crash-looped straight past it. What every reset *does* print is
#     the ROM banner, and only the first boot of a run is a POWERON reset -- so
#     any "rst:0x" line that is not POWERON means the node went down.
#
#  2. Wall-clock limit. --timeout counts *simulated* time, and Wokwi runs well
#     below real time, so a 290s timeout billed 562s. The watcher enforces a
#     wall-clock ceiling itself.
#
# Plus scenario-specific aborts (default: the simulated battery misread, after
# which the cutoff can never arm). Patterns are '|'-separated fixed strings.
crash_on="RTC_SW_CPU_RST|TG0WDT_SYS_RST|TG1WDT_SYS_RST|RTCWDT_RTC_RST|panicked at|Guru Meditation|assert failed|abort() was called"
default_abort="absent (USB-only)|usbPower=1"
case "$scenario" in diag-*) default_abort="" ;; esac
abort_on="${WOKWI_ABORT_ON-$default_abort}"
wall_limit="${WOKWI_WALL_LIMIT_S:-240}"

first_match() {  # first_match <patterns> <file>: print the first pattern found
  printf '%s\n' "$1" | tr '|' '\n' | while IFS= read -r pat; do
    [ -n "$pat" ] && LC_ALL=C grep -aqF -e "$pat" "$2" && { printf '%s' "$pat"; break; }
  done
}

watch_start=$(date +%s)
(
  while sleep 2; do
    reason=""
    if [ -f "$log" ]; then
      m=$(first_match "$crash_on" "$log");  [ -n "$m" ] && reason="node crashed or reset ('$m')"
      [ -z "$reason" ] && { m=$(first_match "$abort_on" "$log"); [ -n "$m" ] && reason="serial log matched '$m'"; }
    fi
    [ -z "$reason" ] && [ $(( $(date +%s) - watch_start )) -ge "$wall_limit" ] && reason="wall-clock limit ${wall_limit}s reached"
    if [ -n "$reason" ]; then
      echo "aborting run: $reason" >&2
      pkill -f "serial-log-file $log" 2>/dev/null
      exit 0
    fi
  done
) &
watcher=$!

. ./.wokwi-env
started=$(date +%s)
~/.local/bin/wokwi-cli \
  --timeout "${WOKWI_TIMEOUT_MS:-290000}" \
  --scenario "$file" \
  --fail-text "Rebooting..." \
  --serial-log-file "$log" \
  .
status=$?
elapsed=$(( $(date +%s) - started ))
kill "$watcher" 2>/dev/null

# Keep a ledger: CI minutes are paid for, and a running total is the only way
# to notice a run that costs more than it should.
printf '%s\t%s\t%ss\texit=%s\t%s\n' "$(date '+%Y-%m-%d %H:%M')" "$scenario" "$elapsed" "$status" "$log" \
  >> .pio/wokwi-logs/ledger.tsv
total=$(awk -F'\t' '{sub(/s$/, "", $3); t += $3} END {printf "%.1f", t / 60}' .pio/wokwi-logs/ledger.tsv)

echo "serial log: $log (exit $status)" >&2
echo "this run: ${elapsed}s wall clock; ledger total: ${total} min" >&2
exit $status
