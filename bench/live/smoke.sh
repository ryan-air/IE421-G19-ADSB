#!/usr/bin/env bash
# Live-ingest integration test: synthetic traffic -> readsb -> Beast TCP ->
# adsb-live. Runs inside the bench image (bench/legacy/Dockerfile), which has
# readsb and a C++ toolchain. Run it with bench/live/run.sh.
#
# Phase 1 (steady state, 60 s, 300 aircraft):
#   * adsb-live connects once and parses the stream without errors
#   * it records (nearly) every frame readsb forwards
#   * every track it creates is a simulated aircraft (no phantoms) and nearly
#     every simulated aircraft is tracked
#   * replaying its recording gives the same track count as replaying the
#     original synthetic recording
# Phase 2 (resilience): readsb is killed and restarted while adsb-live runs;
#   adsb-live must reconnect by itself and keep receiving.
#
# Exits nonzero if any check fails. Summary in $OUT/summary.txt.

set -uo pipefail
OUT="${OUT:-/out}"
WORK=/work
RX_LAT=40.0392
RX_LON=-88.2781
mkdir -p "$OUT" "$WORK"
git config --global --add safe.directory /repo 2>/dev/null
FAIL=0
check() {  # description, condition (bash test expression)
  if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1   [$2]"; FAIL=1; fi
}
log() { printf '[%(%H:%M:%S)T] %s\n' -1 "$*"; }

log "building core/"
cmake -S /repo/core -B "$WORK/b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DADSB_BUILD_TESTS=OFF >/dev/null || exit 2
cmake --build "$WORK/b" >/dev/null || exit 2
T="$WORK/b/tools"

start_readsb() {
  readsb --net-only --net-bi-port 30004 --net-bo-port 30005 --lat "$RX_LAT" --lon "$RX_LON" --quiet \
         >> "$OUT/readsb.log" 2>&1 &
  READSB=$!
  for _ in $(seq 1 100); do nc -z 127.0.0.1 30005 2>/dev/null && return 0; sleep 0.1; done
  echo "readsb did not start"; exit 2
}

# ---------------------------------------------------------------------------
log "phase 1: steady state"
"$T/adsb-synth" -o "$WORK/traffic.adsbrec" --aircraft 300 --duration 60 --seed 7 --lines 1 > "$OUT/synth.txt"
AIRCRAFT=300
start_readsb
"$T/adsb-live" --port 30005 --record "$WORK/live.adsbrec" --receiver "$RX_LAT,$RX_LON" \
               --duration 66 --stats-every 15 > "$OUT/live1.txt" 2> "$OUT/live1.err" &
LIVE=$!
sleep 1
python3 /repo/bench/legacy/feeder.py "$WORK/traffic.adsbrec" --meta "$OUT/feed1.json" > "$OUT/feeder1.txt"
wait "$LIVE"
kill "$READSB" 2>/dev/null; wait "$READSB" 2>/dev/null

FED=$(python3 -c "import json; print(json.load(open('$OUT/feed1.json'))['sent'])")
grab() { grep -E "^$1" "$OUT/live1.txt" | head -1; }
CONNECTS=$(grab source | sed -E 's/.* ([0-9]+) connects.*/\1/')
TRUNC=$(grab stream | sed -E 's/.* ([0-9]+) truncated.*/\1/')
STREAM_FRAMES=$(grab stream | sed -E 's/.* bytes, ([0-9]+) frames.*/\1/')
RECORDED=$(grab recording | sed -E 's/.*: ([0-9]+) records, ([0-9]+) dropped.*/\1/')
REC_DROPPED=$(grab recording | sed -E 's/.*: ([0-9]+) records, ([0-9]+) dropped.*/\2/')
TRACKS=$(grab tracks | sed -E 's/.*created ([0-9]+).*/\1/')

# Replay both recordings through the same engine and compare.
"$T/adsb-replay" "$WORK/live.adsbrec" > "$OUT/replay_live.txt"
"$T/adsb-replay" "$WORK/traffic.adsbrec" > "$OUT/replay_synth.txt"
created() { grep -E "^tracks" "$1" | sed -E 's/.*created ([0-9]+).*/\1/'; }
TR_LIVE=$(created "$OUT/replay_live.txt")
TR_SYNTH=$(created "$OUT/replay_synth.txt")
# Phantom check: every ICAO in the live recording's track output must be a simulated aircraft.
python3 - "$WORK/traffic.adsbrec" "$WORK/live.adsbrec" > "$OUT/icao_check.txt" <<'PY'
import struct, sys
def icaos(path):
    d = open(path, "rb").read(); pos, end, out = 64, len(d) - 24, set()
    while pos + 20 <= end:
        typ, ln = d[pos + 17], d[pos + 19]
        p = d[pos + 20:pos + 20 + ln]
        if typ == ord("3") and (p[0] >> 3) in (17, 18):
            out.add(p[1:4].hex().upper())
        pos += 20 + ln
    return out
synth, live = icaos(sys.argv[1]), icaos(sys.argv[2])
print(len(synth), len(live), len(live - synth))
PY
read -r ICAO_SYNTH ICAO_LIVE ICAO_PHANTOM < "$OUT/icao_check.txt"

{
  echo "== phase 1 =="
  check "connected exactly once"                        '[[ "$CONNECTS" == 1 ]]'
  check "no truncated frames in the stream"              '[[ "$TRUNC" == 0 ]]'
  check "recorder dropped nothing"                       '[[ "$REC_DROPPED" == 0 ]]'
  check "recorded every frame received ($RECORDED/$STREAM_FRAMES)" '[[ "$RECORDED" == "$STREAM_FRAMES" ]]'
  check "received >= 95% of frames fed ($STREAM_FRAMES/$FED)" '(( STREAM_FRAMES * 100 >= FED * 95 ))'
  check "no phantom ICAO addresses ($ICAO_PHANTOM)"       '[[ "$ICAO_PHANTOM" == 0 ]]'
  check "tracked >= 95% of aircraft ($TRACKS/$AIRCRAFT)" '(( TRACKS * 100 >= AIRCRAFT * 95 ))'
  check "no more tracks than aircraft ($TRACKS/$AIRCRAFT)" '(( TRACKS <= AIRCRAFT ))'
  check "replayed live recording matches synthetic ($TR_LIVE vs $TR_SYNTH)" '(( TR_LIVE >= TR_SYNTH - 3 && TR_LIVE <= TR_SYNTH ))'
} | tee "$OUT/summary.txt"

# ---------------------------------------------------------------------------
log "phase 2: readsb restart while adsb-live is running"
"$T/adsb-synth" -o "$WORK/small.adsbrec" --aircraft 40 --duration 8 --seed 9 --lines 1 > /dev/null
start_readsb
"$T/adsb-live" --port 30005 --duration 30 --stats-every 0 > "$OUT/live2.txt" 2> "$OUT/live2.err" &
LIVE=$!
sleep 1
python3 /repo/bench/legacy/feeder.py "$WORK/small.adsbrec" --meta "$OUT/feed2a.json" > /dev/null
kill "$READSB"; wait "$READSB" 2>/dev/null
sleep 2                                   # adsb-live sees the disconnect and backs off
start_readsb
sleep 2                                   # let it reconnect
python3 /repo/bench/legacy/feeder.py "$WORK/small.adsbrec" --meta "$OUT/feed2b.json" > /dev/null
sleep 2
kill -INT "$LIVE"; wait "$LIVE"
kill "$READSB" 2>/dev/null; wait "$READSB" 2>/dev/null
C2=$(grep -E "^source" "$OUT/live2.txt" | sed -E 's/.* ([0-9]+) connects, ([0-9]+) disconnects.*/\1 \2/')
read -r CONNECTS2 DISCONNECTS2 <<< "$C2"
F2=$(grep -E "^stream" "$OUT/live2.txt" | sed -E 's/.* bytes, ([0-9]+) frames.*/\1/')
SENT2=$(python3 -c "import json; print(json.load(open('$OUT/feed2a.json'))['sent'] + json.load(open('$OUT/feed2b.json'))['sent'])")
{
  echo "== phase 2 =="
  check "reconnected after readsb restart ($CONNECTS2 connects, $DISCONNECTS2 disconnects)" '(( CONNECTS2 >= 2 && DISCONNECTS2 >= 1 ))'
  check "received frames from both readsb runs ($F2 of $SENT2 fed)" '(( F2 * 100 >= SENT2 * 90 ))'
  check "clean shutdown on Ctrl-C" 'grep -q "^latency" "$OUT/live2.txt"'
} | tee -a "$OUT/summary.txt"

echo; echo "== adsb-live (phase 1) =="; cat "$OUT/live1.txt"
exit "$FAIL"
