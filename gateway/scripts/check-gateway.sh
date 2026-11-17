#!/usr/bin/env bash
# Gateway integration test against the real C++ publisher: every gateway's
# book, and the table of every WebSocket client behind it, must be
# byte-identical to the publisher's book.
#
#   gateway/scripts/check-gateway.sh CORE_BUILD_DIR
#
# Scenario 1 (one publisher, multicast, publish timestamps on):
#   GA  gateway from the start, no extra loss; two WebSocket clients behind
#       it: a normal one and a slow one (stalls 3 ms per frame), which must
#       be conflated, not queued: its median latency must stay within 100 ms
#       of the normal client's (without flow control it is seconds behind)
#   GB  gateway losing bursts of 4,000 datagrams locally: gaps outrun the
#       2,048-message ring, so it must recover by TCP snapshot
#   GC  gateway joining 1.5 s late: snapshot, then live
#   The publisher drops 1% of datagrams (bursts of 3) and 5% of retransmit
#   replies.
# Scenario 2 (publisher restart, no publish timestamps): a gateway and a
#   client across two publisher runs end with the second run's book, which
#   also equals a plain deterministic replay of that recording.
#
# Environment: FEED_GROUP (default 239.19.0.8), FEED_IFACE (default
# 127.0.0.1), FEED_PORT (base port, default 42950), HTTP_PORT (base, default
# 18180), KEEP=1 keeps the work directory. Exits nonzero if any check fails.

set -uo pipefail
BUILD="${1:?usage: check-gateway.sh CORE_BUILD_DIR}"
T="$BUILD/tools"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
GW=(node "$HERE/bin/adsb-gateway.mjs")
WSC=(node "$HERE/bin/adsb-ws-client.mjs" --clock-helper "$T/adsb-clock")
for t in adsb-synth adsb-replay adsb-publish adsb-clock; do
  [[ -x "$T/$t" ]] || { echo "missing $T/$t (build core with tools enabled)"; exit 2; }
done
GROUP="${FEED_GROUP:-239.19.0.8}"
IFACE="${FEED_IFACE:-127.0.0.1}"
PORT="${FEED_PORT:-42950}"
RTX=$((PORT + 1))
SNAP=$((PORT + 2))
HP="${HTTP_PORT:-18180}"
W="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; [[ -n "${KEEP:-}" ]] && echo "kept $W" || rm -rf "$W"' EXIT
FAIL=0
check() {  # description, condition
  if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1   [$2]"; FAIL=1; fi
}
digest() { grep -E '^book digest' "$1" | awk '{print $3}'; }
sub_field() { grep -E '^subscriber ' "$1" | sed -E "s/.*\"$2\":([0-9]+).*/\1/"; }
frames() { grep -E '^frames ' "$1" | awk '{print $2}'; }
messages() { grep -E '^frames ' "$1" | sed -E 's/.* messages ([0-9]+).*/\1/'; }

PUB_COMMON=(--feed "$GROUP:$PORT" --interface "$IFACE" --retransmit-port "$RTX" --snapshot-port "$SNAP"
            --bind 127.0.0.1 --no-flush --heartbeat-ms 50)
GW_COMMON=(--feed "$GROUP:$PORT" --interface "$IFACE" --retransmit "127.0.0.1:$RTX"
           --snapshot "127.0.0.1:$SNAP" --clock-helper "$T/adsb-clock" --quiet)

echo "== scenario 1: loss, small ring, late join, slow client =="
"$T/adsb-synth" -o "$W/a.adsbrec" --aircraft 300 --duration 60 --seed 7 > /dev/null || exit 2

"${GW[@]}" "${GW_COMMON[@]}" --listen "127.0.0.1:$HP" --until-end --duration 45 > "$W/GA.txt" 2>&1 &
PGA=$!
"${GW[@]}" "${GW_COMMON[@]}" --listen "127.0.0.1:$((HP + 1))" --until-end --duration 45 \
    --drop-rate 0.0002 --drop-burst 4000 --drop-only-first 25000 --seed 3 > "$W/GB.txt" 2>&1 &
PGB=$!
sleep 1
"${WSC[@]}" --url "ws://127.0.0.1:$HP/gateway/ws" --until-end --duration 45 > "$W/CA.txt" 2>&1 &
PCA=$!
"${WSC[@]}" --url "ws://127.0.0.1:$HP/gateway/ws" --until-end --duration 45 --slow-ms 3 > "$W/CS.txt" 2>&1 &
PCS=$!
"${WSC[@]}" --url "ws://127.0.0.1:$((HP + 1))/gateway/ws" --until-end --duration 45 > "$W/CB.txt" 2>&1 &
PCB=$!
sleep 0.5
"$T/adsb-publish" "$W/a.adsbrec" "${PUB_COMMON[@]}" --session GWRA000001 --speed 15 --ring 2048 \
    --drop-rate 0.01 --drop-burst 3 --reply-drop-rate 0.05 --linger 5 > "$W/P.txt" &
PP=$!
sleep 1.5
"${GW[@]}" "${GW_COMMON[@]}" --listen "127.0.0.1:$((HP + 2))" --until-end --duration 45 > "$W/GC.txt" 2>&1 &
PGC=$!
sleep 0.5
"${WSC[@]}" --url "ws://127.0.0.1:$((HP + 2))/gateway/ws" --until-end --duration 45 > "$W/CC.txt" 2>&1 &
PCC=$!
wait $PP; RP=$?
for v in GA GB GC CA CS CB CC; do
  p="P$v"; wait "${!p}"; eval "R$v=$?"
done

DP=$(digest "$W/P.txt")
check "publisher ran ($RP)"                                   '[[ $RP == 0 && -n "$DP" ]]'
for g in GA GB GC; do
  check "$g ended and recovered (exit $(eval echo \$R$g))" "[[ \$R$g == 0 ]]"
  check "$g book == publisher"                  "[[ \"\$(digest \"\$W/$g.txt\")\" == \"\$DP\" ]]"
done
check "GA recovered gaps by retransmit ($(sub_field "$W/GA.txt" requests) requests)" '(( $(sub_field "$W/GA.txt" requests) > 0 ))'
check "GB needed snapshots ($(sub_field "$W/GB.txt" snapshots_loaded))"            '(( $(sub_field "$W/GB.txt" snapshots_loaded) >= 1 ))'
check "GC joined by snapshot ($(sub_field "$W/GC.txt" snapshots_loaded))"          '(( $(sub_field "$W/GC.txt" snapshots_loaded) >= 1 ))'
for c in CA CS CB CC; do
  check "client $c saw the end ($(eval echo \$R$c))" "[[ \$R$c == 0 ]]"
  check "client $c table == publisher"           "[[ \"\$(digest \"\$W/$c.txt\")\" == \"\$DP\" ]]"
done
check "slow client was conflated ($(messages "$W/CS.txt") vs $(messages "$W/CA.txt") messages)" \
      '(( $(messages "$W/CS.txt") < $(messages "$W/CA.txt") ))'
# Median, not p99: tails on a busy laptop reach 100+ ms for every client.
# Without flow control this median is over a second (the backlog sits in the
# kernel socket buffers); with it, about window x 3 ms.
p50_ms() { grep -E '^publish -> client' "$1" | sed -E 's/.* p50 ([0-9]+|Infinity) .*/\1/' | awk '{ print ($1 == "Infinity") ? 999999 : int($1 / 1000) }'; }
# Relative to the normal client, so a loaded machine (which slows every
# client) does not fail the check.
check "slow client stayed current: p50 $(p50_ms "$W/CS.txt") ms vs normal $(p50_ms "$W/CA.txt") ms (within 100)" \
      '(( $(p50_ms "$W/CS.txt") < $(p50_ms "$W/CA.txt") + 100 ))'
echo "     normal $(grep -E '^publish -> client' "$W/CA.txt")"
echo "     slow   $(grep -E '^publish -> client' "$W/CS.txt")"
echo "     GA $(grep -E '^publish -> websocket out' "$W/GA.txt")"

echo "== scenario 2: publisher restart =="
"$T/adsb-synth" -o "$W/b.adsbrec" --aircraft 120 --duration 20 --seed 8 > /dev/null || exit 2
"$T/adsb-replay" "$W/b.adsbrec" --no-flush --book-digest > "$W/replay.txt" || exit 2
"${GW[@]}" "${GW_COMMON[@]}" --listen "127.0.0.1:$((HP + 3))" --duration 14 > "$W/GR.txt" 2>&1 &
PGR=$!
sleep 1
"${WSC[@]}" --url "ws://127.0.0.1:$((HP + 3))/gateway/ws" --duration 12 > "$W/CR.txt" 2>&1 &
PCR=$!
sleep 0.5
"$T/adsb-publish" "$W/a.adsbrec" "${PUB_COMMON[@]}" --no-publish-ts --session GWRB000001 --speed 30 --linger 0.5 > "$W/P1.txt"
"$T/adsb-publish" "$W/b.adsbrec" "${PUB_COMMON[@]}" --no-publish-ts --session GWRB000002 --speed 20 --linger 1 > "$W/P2.txt"
wait $PCR
wait $PGR
D2=$(digest "$W/P2.txt")
check "second run's book == deterministic replay"          '[[ -n "$D2" && "$D2" == "$(digest "$W/replay.txt")" ]]'
check "gateway followed the restart ($(sub_field "$W/GR.txt" session_changes) change)" '[[ "$(sub_field "$W/GR.txt" session_changes)" == 1 ]]'
check "gateway book == the second run's book"              '[[ "$(digest "$W/GR.txt")" == "$D2" ]]'
check "client table == the second run's book"              '[[ "$(digest "$W/CR.txt")" == "$D2" ]]'
check "and differs from the first run's"                   '[[ "$(digest "$W/CR.txt")" != "$(digest "$W/P1.txt")" ]]'
check "client knows session GWRB000002 ended"              'grep -q "ended true session GWRB000002" "$W/CR.txt"'

if (( FAIL )); then
  for f in P GA GB GC CA CS CB CC P1 P2 GR CR; do echo "--- $f"; cat "$W/$f.txt"; done
fi
exit "$FAIL"
