#!/usr/bin/env bash
# Feed integration test (README_UPDATES.md §9.5 item 4): the reconstructed
# book of every subscriber must be byte-identical to the publisher's, under
# injected loss, a small retransmit ring, a late join, and a publisher restart.
#
#   core/scripts/check-feed.sh BUILD_DIR
#
# Scenario 1 (one publisher, three subscribers, multicast):
#   A  joins at the start, no extra loss
#   B  loses bursts of 4,000 datagrams locally: gaps outrun the 2,048-message
#      ring, so it must recover through TCP snapshots. (Injection stops after
#      25,000 datagrams: a burst that swallowed the tail and every repeat of
#      the end-of-session marker would leave B, correctly, unable to know the
#      session ended.)
#   C  joins 1.5 s late: snapshot, then live
#   The publisher itself drops 1% of datagrams (bursts of 3) and 5% of
#   retransmit replies. All three digests must equal the publisher's AND the
#   digest of a plain deterministic replay of the same recording.
# Scenario 2 (publisher restart): one subscriber across two publisher runs
#   with different sessions ends with the second run's book.
#
# Environment: FEED_GROUP (default 239.19.0.7), FEED_IFACE (default
# 127.0.0.1; use 0.0.0.0 where loopback multicast is unavailable), FEED_PORT
# (base port, default 42900), FEED_TRANSPORT (the publisher's UDP transport:
# kernel (default), uring, uring-nosqpoll; with io_uring every publisher must
# also report 0 reordered completions). Exits nonzero if any check fails.

set -uo pipefail
BUILD="${1:?usage: check-feed.sh BUILD_DIR}"
T="$BUILD/tools"
for t in adsb-synth adsb-replay adsb-publish adsb-feed-cli; do
  [[ -x "$T/$t" ]] || { echo "missing $T/$t (build with tools enabled)"; exit 2; }
done
GROUP="${FEED_GROUP:-239.19.0.7}"
IFACE="${FEED_IFACE:-127.0.0.1}"
PORT="${FEED_PORT:-42900}"
RTX=$((PORT + 1))
SNAP=$((PORT + 2))
W="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; [[ -n "${KEEP:-}" ]] || rm -rf "$W"' EXIT
FAIL=0
check() {  # description, condition
  if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1   [$2]"; FAIL=1; fi
}
digest() { grep -E '^book digest' "$1" | awk '{print $3}'; }
field() { grep -E "^$2" "$1" | head -1; }

TRANSPORT="${FEED_TRANSPORT:-kernel}"
PUB_COMMON=(--feed "$GROUP:$PORT" --interface "$IFACE" --retransmit-port "$RTX" --snapshot-port "$SNAP"
            --bind 127.0.0.1 --no-flush --no-publish-ts --heartbeat-ms 50 --transport "$TRANSPORT")
SUB_COMMON=(--feed "$GROUP:$PORT" --interface "$IFACE" --retransmit "127.0.0.1:$RTX"
            --snapshot "127.0.0.1:$SNAP" --quiet)

echo "== scenario 1: loss, small ring, late join =="
"$T/adsb-synth" -o "$W/a.adsbrec" --aircraft 300 --duration 60 --seed 7 > /dev/null || exit 2
"$T/adsb-replay" "$W/a.adsbrec" --no-flush --book-digest > "$W/replay.txt" || exit 2
D0=$(digest "$W/replay.txt")

"$T/adsb-feed-cli" "${SUB_COMMON[@]}" --until-end --duration 40 > "$W/A.txt" &
PA=$!
"$T/adsb-feed-cli" "${SUB_COMMON[@]}" --until-end --duration 40 --drop-rate 0.0002 --drop-burst 4000 --drop-only-first 25000 --seed 3 > "$W/B.txt" &
PB=$!
sleep 0.5
"$T/adsb-publish" "$W/a.adsbrec" "${PUB_COMMON[@]}" --session RUNA000001 --speed 15 --ring 2048 \
    --drop-rate 0.01 --drop-burst 3 --reply-drop-rate 0.05 --linger 4 > "$W/P.txt" &
PP=$!
sleep 1.5
"$T/adsb-feed-cli" "${SUB_COMMON[@]}" --until-end --duration 40 > "$W/C.txt" &
PC=$!
wait $PP; RP=$?
wait $PA; RA=$?
wait $PB; RB=$?
wait $PC; RC=$?

DP=$(digest "$W/P.txt")
snapshots() { field "$1" recovery | sed -E 's/.* ([0-9]+) snapshots loaded.*/\1/'; }
requests() { field "$1" recovery | sed -E 's/.* ([0-9]+) requests.*/\1/'; }
check "publisher ran ($RP)"                                     '[[ $RP == 0 ]]'
check "publisher book == deterministic replay ($DP)"            '[[ -n "$DP" && "$DP" == "$D0" ]]'
check "A ended and recovered ($RA)"                             '[[ $RA == 0 ]]'
check "A book == publisher"                                     '[[ "$(digest "$W/A.txt")" == "$DP" ]]'
check "A recovered gaps by retransmit ($(requests "$W/A.txt") requests)" '(( $(requests "$W/A.txt") > 0 ))'
check "B ended and recovered ($RB)"                             '[[ $RB == 0 ]]'
check "B book == publisher"                                     '[[ "$(digest "$W/B.txt")" == "$DP" ]]'
check "B needed snapshots ($(snapshots "$W/B.txt"))"            '(( $(snapshots "$W/B.txt") >= 1 ))'
check "C (late joiner) ended and recovered ($RC)"               '[[ $RC == 0 ]]'
check "C book == publisher"                                     '[[ "$(digest "$W/C.txt")" == "$DP" ]]'
check "C joined by snapshot ($(snapshots "$W/C.txt"))"          '(( $(snapshots "$W/C.txt") >= 1 ))'

echo "== scenario 2: publisher restart =="
"$T/adsb-synth" -o "$W/b.adsbrec" --aircraft 120 --duration 20 --seed 8 > /dev/null || exit 2
"$T/adsb-feed-cli" "${SUB_COMMON[@]}" --duration 12 > "$W/R.txt" &
PR=$!
sleep 0.5
"$T/adsb-publish" "$W/a.adsbrec" "${PUB_COMMON[@]}" --session RUNB000001 --speed 30 --linger 0.5 > "$W/P1.txt"
"$T/adsb-publish" "$W/b.adsbrec" "${PUB_COMMON[@]}" --session RUNB000002 --speed 20 --linger 1 > "$W/P2.txt"
wait $PR
CHANGES=$(field "$W/R.txt" recovery | sed -E 's/.* ([0-9]+) session changes.*/\1/')
check "subscriber followed the restart ($CHANGES session change)" '[[ "$CHANGES" == 1 ]]'
check "its book == the second run's book"                      '[[ "$(digest "$W/R.txt")" == "$(digest "$W/P2.txt")" ]]'
check "and differs from the first run's"                       '[[ "$(digest "$W/R.txt")" != "$(digest "$W/P1.txt")" ]]'
check "session RUNB000002 reached its end"                     'grep -q "RUNB000002, ended" "$W/R.txt"'

if [[ $TRANSPORT != kernel ]]; then
  echo "== io_uring ($TRANSPORT) =="
  for f in P P1 P2; do
    line=$(grep '^io_uring' "$W/$f.txt")
    check "publisher $f sent through io_uring, 0 reordered, 0 failed: ${line#io_uring    }" \
      '[[ -n "$line" ]] && grep -q " 0 failed" <<<"$line" && grep -q " 0 reordered" <<<"$line"'
  done
fi

if (( FAIL )); then
  for f in P A B C P1 P2 R; do echo "--- $f"; cat "$W/$f.txt"; done
fi
exit "$FAIL"
