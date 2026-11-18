#!/usr/bin/env bash
# Shared-memory transport integration test (README_UPDATES.md §9.3): every
# subscriber on the ring must end with a book byte-identical to the
# publisher's, whether it keeps up, is lapped, joins late, or sees the
# publisher restart.
#
#   core/scripts/check-shm.sh BUILD_DIR
#
# Scenario 1 (one publisher on UDP and a 2,048-message ring):
#   A  joins at the start and keeps up (no snapshot, no lap)
#   B  spins 1 ms per message (1k messages/s, against ~4.5k published): the writer laps it, it resyncs by snapshot
#   C  joins after the ring has wrapped: snapshot, then live
# Scenario 2 (publisher restart): one subscriber across two publisher runs
#   that replace the ring follows to the second session and ends with its book.
#
# Environment: SHM_PORT (base port, default 44900). KEEP=1 keeps the work dir.

set -uo pipefail
BUILD="${1:?usage: check-shm.sh BUILD_DIR}"
T="$BUILD/tools"
for t in adsb-synth adsb-publish adsb-shm-cli; do
  [[ -x "$T/$t" ]] || { echo "missing $T/$t (build with tools enabled)"; exit 2; }
done
PORT="${SHM_PORT:-44900}"
NAME="/adsb-check-$$"
W="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; [[ -n "${KEEP:-}" ]] && echo "kept $W" || rm -rf "$W"' EXIT
FAIL=0
check() {  # description, condition
  if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1   [$2]"; FAIL=1; fi
}
digest() { grep -E '^book digest' "$1" | awk '{print $3}'; }
stat_of() { grep -E '^stats' "$1" | tail -1 | sed -E "s/.* ([0-9]+) $2.*/\1/"; }

PUB=(--feed "127.0.0.1:$PORT" --retransmit-port $((PORT + 1)) --snapshot-port $((PORT + 2))
     --bind 127.0.0.1 --no-flush --heartbeat-ms 50 --shm "$NAME" --shm-slots 2048)
SUB=(--name "$NAME" --snapshot "127.0.0.1:$((PORT + 2))" --quiet)

echo "== scenario 1: keep up, lapped, late join =="
"$T/adsb-synth" -o "$W/a.adsbrec" --aircraft 300 --duration 60 --seed 7 > /dev/null || exit 2
"$T/adsb-shm-cli" "${SUB[@]}" --until-end --duration 40 > "$W/A.txt" &
PA=$!
"$T/adsb-shm-cli" "${SUB[@]}" --until-end --duration 40 --slow-ns 1000000 > "$W/B.txt" &
PB=$!
sleep 0.3
"$T/adsb-publish" "$W/a.adsbrec" "${PUB[@]}" --session SHMA000001 --speed 8 --linger 6 > "$W/P.txt" &
PP=$!
sleep 1.5
"$T/adsb-shm-cli" "${SUB[@]}" --until-end --duration 40 > "$W/C.txt" &
PC=$!
wait $PP; RP=$?
wait $PA; RA=$?
wait $PB; RB=$?
wait $PC; RC=$?
DP=$(digest "$W/P.txt")
check "publisher ran ($RP)"                                       '[[ $RP == 0 && -n "$DP" ]]'
for s in A B C; do
  check "$s ended and recovered (exit $(eval echo \$R$s))"        "[[ \$R$s == 0 ]]"
  check "$s book == publisher"                                     "[[ \"\$(digest \$W/$s.txt)\" == \"\$DP\" ]]"
done
check "A kept up: no laps, no snapshots ($(stat_of "$W/A.txt" laps) laps)" \
      '(( $(stat_of "$W/A.txt" laps) == 0 && $(stat_of "$W/A.txt" snapshots) == 0 ))'
check "B was lapped and resynced ($(stat_of "$W/B.txt" laps) laps, $(stat_of "$W/B.txt" snapshots) snapshots)" \
      '(( $(stat_of "$W/B.txt" laps) >= 1 && $(stat_of "$W/B.txt" snapshots) >= 1 ))'
check "C joined by snapshot ($(stat_of "$W/C.txt" snapshots))"    '(( $(stat_of "$W/C.txt" snapshots) >= 1 ))'

echo "== scenario 2: publisher restart replaces the ring =="
"$T/adsb-synth" -o "$W/b.adsbrec" --aircraft 120 --duration 20 --seed 8 > /dev/null || exit 2
"$T/adsb-shm-cli" "${SUB[@]}" --duration 14 > "$W/R.txt" &
PR=$!
sleep 0.3
"$T/adsb-publish" "$W/a.adsbrec" "${PUB[@]}" --session SHMB000001 --speed 30 --linger 0.5 > "$W/P1.txt"
"$T/adsb-publish" "$W/b.adsbrec" "${PUB[@]}" --session SHMB000002 --speed 20 --linger 1 > "$W/P2.txt"
wait $PR
check "subscriber followed the restart ($(stat_of "$W/R.txt" rings) rings opened)" '(( $(stat_of "$W/R.txt" rings) == 2 ))'
check "its book == the second run's book"                         '[[ "$(digest "$W/R.txt")" == "$(digest "$W/P2.txt")" ]]'
check "session SHMB000002 reached its end"                        'grep -q "SHMB000002, ended" "$W/R.txt"'

if (( FAIL )); then
  for f in P A B C P1 P2 R; do echo "--- $f"; cat "$W/$f.txt"; done
fi
exit "$FAIL"
