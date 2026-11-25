#!/usr/bin/env bash
# Conflated feed integration test (README_UPDATES.md §10.2): a subscriber on
# the conflated feed must end with the same aircraft state as the main feed's
# publisher (compared by state digest: the conflated feed carries union masks
# and its own publish times), through loss, a late join, and from both
# publishing tools.
#
#   core/scripts/check-conflate.sh BUILD_DIR
#
# Scenario 1 (adsb-publish, multicast, conflated at 2 Hz on recording time):
#   M   main feed, no extra loss
#   CA  conflated feed, no extra loss
#   CB  conflated feed, losing bursts of datagrams: recovers through the
#       conflated feed's own retransmit (and snapshot) servers
#   CC  conflated feed, joining late: snapshot, then live
#   The conflated feed must also send fewer updates than the main feed.
# Scenario 2 (adsb-live fed by adsb-beast-serve, both feeds, 10 Hz live):
#   a subscriber on each feed ends with the same state as adsb-live.
# Scenario 3 (deletes): every track expires at the end of the run, so
#   TrackDeletes cross the conflated feed, its retransmits (a lossy
#   subscriber) and its snapshots (a late joiner); all books end empty and
#   equal. Also: the conflated feed is the same at 10x and at full speed
#   (ticks run on recording time).
#
# Environment: FEED_GROUP (default 239.19.0.9), FEED_IFACE (default
# 127.0.0.1), FEED_PORT (base port, default 29900: below the ephemeral port
# ranges of Linux (32768-60999) and macOS (49152-65535), so no client socket
# opened meanwhile can hold one of the feed ports). KEEP=1 keeps the work dir.

set -uo pipefail
BUILD="${1:?usage: check-conflate.sh BUILD_DIR}"
T="$BUILD/tools"
for t in adsb-synth adsb-publish adsb-feed-cli adsb-live adsb-beast-serve; do
  [[ -x "$T/$t" ]] || { echo "missing $T/$t (build with tools enabled)"; exit 2; }
done
GROUP="${FEED_GROUP:-239.19.0.9}"
CGROUP="${GROUP%.*}.$(( ${GROUP##*.} + 1 ))"
IFACE="${FEED_IFACE:-127.0.0.1}"
PORT="${FEED_PORT:-29900}"
W="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; [[ -n "${KEEP:-}" ]] && echo "kept $W" || rm -rf "$W"' EXIT
FAIL=0
check() {  # description, condition
  if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1   [$2]"; FAIL=1; fi
}
state() { grep -E "^$2" "$1" | head -1 | awk '{print $NF}'; }
conf_count() { grep -E '^conflated +[0-9]+ updates in' "$1" | sed -E "s/^conflated +([0-9]+) updates in -> ([0-9]+) out.*/\\$2/"; }

MAIN_SUB=(--feed "$GROUP:$PORT" --interface "$IFACE" --retransmit "127.0.0.1:$((PORT + 1))"
          --snapshot "127.0.0.1:$((PORT + 2))" --quiet)
CONF_SUB=(--feed "$CGROUP:$((PORT + 10))" --interface "$IFACE" --retransmit "127.0.0.1:$((PORT + 11))"
          --snapshot "127.0.0.1:$((PORT + 12))" --quiet)

echo "== scenario 1: adsb-publish, conflated at 2 Hz =="
"$T/adsb-synth" -o "$W/a.adsbrec" --aircraft 300 --duration 60 --seed 7 > /dev/null || exit 2
"$T/adsb-feed-cli" "${MAIN_SUB[@]}" --until-end --duration 40 > "$W/M.txt" &
PM=$!
"$T/adsb-feed-cli" "${CONF_SUB[@]}" --until-end --duration 40 > "$W/CA.txt" &
PCA=$!
"$T/adsb-feed-cli" "${CONF_SUB[@]}" --until-end --duration 40 --drop-rate 0.05 --drop-burst 5 \
    --drop-only-first 400 --seed 4 > "$W/CB.txt" &
PCB=$!
sleep 0.5
"$T/adsb-publish" "$W/a.adsbrec" --feed "$GROUP:$PORT" --interface "$IFACE" --bind 127.0.0.1 \
    --retransmit-port $((PORT + 1)) --snapshot-port $((PORT + 2)) --no-flush --heartbeat-ms 50 \
    --conflate "$CGROUP:$((PORT + 10))" --conflate-hz 2 --conflate-retransmit-port $((PORT + 11)) \
    --conflate-snapshot-port $((PORT + 12)) --speed 10 --linger 4 > "$W/P.txt" &
PP=$!
sleep 2
"$T/adsb-feed-cli" "${CONF_SUB[@]}" --until-end --duration 40 > "$W/CC.txt" &
PCC=$!
wait $PP; RP=$?
for s in M CA CB CC; do p="P$s"; wait "${!p}"; eval "R$s=$?"; done
DP=$(state "$W/P.txt" 'state digest')
check "publisher ran ($RP)"                                  '[[ $RP == 0 && -n "$DP" ]]'
check "the conflated publisher's book matches the main one"  '[[ "$(state "$W/P.txt" "conflated state digest")" == "$DP" ]]'
check "conflated feed sent fewer updates ($(conf_count "$W/P.txt" 2) of $(conf_count "$W/P.txt" 1))" \
      '(( $(conf_count "$W/P.txt" 2) < $(conf_count "$W/P.txt" 1) ))'
for s in M CA CB CC; do
  check "$s ended and recovered (exit $(eval echo \$R$s))"    "[[ \$R$s == 0 ]]"
  check "$s state == publisher's"                             "[[ \"\$(state \$W/$s.txt 'state digest')\" == \"\$DP\" ]]"
done
snapshots() { grep -E '^recovery' "$1" | sed -E 's/.* ([0-9]+) snapshots loaded.*/\1/'; }
check "CC joined by snapshot ($(snapshots "$W/CC.txt"))"     '(( $(snapshots "$W/CC.txt") >= 1 ))'
check "CB recovered gaps on the conflated feed ($(grep -oE '[0-9]+ requests' "$W/CB.txt" | head -1))" \
      'grep -qE "[1-9][0-9]* requests" "$W/CB.txt"'

echo "== scenario 2: adsb-live, both feeds, 10 Hz =="
"$T/adsb-feed-cli" "${MAIN_SUB[@]}" --until-end --duration 30 > "$W/LM.txt" &
PLM=$!
"$T/adsb-feed-cli" "${CONF_SUB[@]}" --until-end --duration 30 > "$W/LC.txt" &
PLC=$!
"$T/adsb-beast-serve" "$W/a.adsbrec" --port $((PORT + 20)) --speed 10 > "$W/serve.txt" 2>&1 &
PS=$!
sleep 0.5
"$T/adsb-live" --port $((PORT + 20)) --quiet --stats-every 0 \
    --publish "$GROUP:$PORT" --publish-interface "$IFACE" --retransmit-port $((PORT + 1)) --snapshot-port $((PORT + 2)) \
    --conflate "$CGROUP:$((PORT + 10))" --conflate-retransmit-port $((PORT + 11)) \
    --conflate-snapshot-port $((PORT + 12)) > "$W/L.txt" 2>&1 &
PL=$!
# adsb-beast-serve ends when its client has read the recording: if adsb-live
# exits first (it failed to start), stop the server instead of waiting on it.
while kill -0 $PS 2>/dev/null; do
  if ! kill -0 $PL 2>/dev/null; then echo "adsb-live exited early"; kill $PS 2>/dev/null; break; fi
  sleep 0.2
done
wait $PS
sleep 0.5
kill -INT $PL 2>/dev/null; wait $PL; RL=$?
wait $PLM; RLM=$?
wait $PLC; RLC=$?
DL=$(state "$W/L.txt" 'state digest')
check "adsb-live ran ($RL)"                                   '[[ $RL == 0 && -n "$DL" ]]'
[[ $RL == 0 && -n "$DL" ]] || { echo "adsb-live output:"; cat "$W/L.txt"; }
check "its conflated book matches its main book"              '[[ "$(state "$W/L.txt" "conflated state digest")" == "$DL" ]]'
check "main subscriber ended and matches ($RLM)"              '[[ $RLM == 0 && "$(state "$W/LM.txt" "state digest")" == "$DL" ]]'
check "conflated subscriber ended and matches ($RLC)"         '[[ $RLC == 0 && "$(state "$W/LC.txt" "state digest")" == "$DL" ]]'

echo "== scenario 3: deletes on the conflated feed; determinism =="
"$T/adsb-synth" -o "$W/d.adsbrec" --aircraft 150 --duration 30 --seed 9 > /dev/null || exit 2
"$T/adsb-feed-cli" "${CONF_SUB[@]}" --until-end --duration 30 --drop-rate 0.05 --drop-burst 3 \
    --drop-only-first 300 --seed 6 > "$W/DL.txt" &
PDL=$!
sleep 0.5
"$T/adsb-publish" "$W/d.adsbrec" --feed "$GROUP:$PORT" --interface "$IFACE" --bind 127.0.0.1 \
    --retransmit-port $((PORT + 1)) --snapshot-port $((PORT + 2)) --heartbeat-ms 50 \
    --conflate "$CGROUP:$((PORT + 10))" --conflate-hz 2 --conflate-retransmit-port $((PORT + 11)) \
    --conflate-snapshot-port $((PORT + 12)) --speed 10 --linger 3 > "$W/D.txt" &
PD=$!
sleep 2.5
"$T/adsb-feed-cli" "${CONF_SUB[@]}" --until-end --duration 30 > "$W/DJ.txt" &
PDJ=$!
wait $PD; RD=$?
wait $PDL; RDL=$?
wait $PDJ; RDJ=$?
DD=$(state "$W/D.txt" 'state digest')
dels() { grep -E '^conflated +[0-9]+ updates in' "$1" | sed -E 's/.* ([0-9]+) deletes out.*/\1/'; }
check "publisher ran ($RD)"                                   '[[ $RD == 0 && -n "$DD" ]]'
check "deletes crossed the conflated feed ($(dels "$W/D.txt") deletes out)" '(( $(dels "$W/D.txt") > 0 ))'
check "the conflated publisher's book is empty and matches"   '[[ "$(state "$W/D.txt" "conflated state digest")" == "$DD" ]]'
check "lossy subscriber ended, state matches ($RDL)"          '[[ $RDL == 0 && "$(state "$W/DL.txt" "state digest")" == "$DD" ]]'
check "late joiner ended, state matches ($RDJ)"               '[[ $RDJ == 0 && "$(state "$W/DJ.txt" "state digest")" == "$DD" ]]'
# The same recording at full speed: the conflated stream must not depend on pacing.
"$T/adsb-publish" "$W/d.adsbrec" --feed "$GROUP:$PORT" --interface "$IFACE" --bind 127.0.0.1 \
    --retransmit-port $((PORT + 1)) --snapshot-port $((PORT + 2)) --heartbeat-ms 50 \
    --conflate "$CGROUP:$((PORT + 10))" --conflate-hz 2 --conflate-retransmit-port $((PORT + 11)) \
    --conflate-snapshot-port $((PORT + 12)) --speed max --linger 0 > "$W/DM.txt"
counts() { grep -E '^conflated +[0-9]+ updates in' "$1" | sed -E 's/.*-> ([0-9]+) out.*, ([0-9]+) deletes out.*, ([0-9]+) ticks.*/\1 \2 \3/'; }
check "conflated stream identical at 10x and full speed ($(counts "$W/D.txt") vs $(counts "$W/DM.txt"))" \
      '[[ -n "$(counts "$W/D.txt")" && "$(counts "$W/D.txt")" == "$(counts "$W/DM.txt")" ]]'

if (( FAIL )); then
  for f in P M CA CB CC L LM LC D DL DJ DM; do echo "--- $f"; cat "$W/$f.txt"; done
fi
exit "$FAIL"
