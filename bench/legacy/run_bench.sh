#!/usr/bin/env bash
# Runs inside the `bench` container (see docker-compose.yml). For each fleet
# size it generates synthetic traffic, then plays the same recording in real
# time through each system, one at a time, and probes what the map would show:
#
#   BEFORE  synthetic Beast --> readsb --SBS-1:30003--> Fall 2025 streamer --> Postgres
#                                                   --> Fall 2025 Node server --> /api/aircraft (polled)
#   AFTER   synthetic Beast --> readsb --Beast:30005--> adsb-live --multicast feed--> adsb-gateway
#                                                   --> WebSocket push (and /api/aircraft from memory)
#
# Finally the core of the new pipeline is replayed alone (no network) for context.
#
# Environment:
#   SCENARIOS  fleet sizes to run         (default "50 200 1000 3000")
#   DURATION   seconds of traffic per run (default 120)
#   SEED       synthetic traffic seed     (default 2025)
#   KEEP_RAW   1 = keep each run's raw logs after analysis (they reach several
#              hundred MB per run at high load; default 0 deletes them)

set -uo pipefail

SCENARIOS="${SCENARIOS:-50 200 1000 3000}"
DURATION="${DURATION:-120}"
SEED="${SEED:-2025}"
KEEP_RAW="${KEEP_RAW:-0}"
DRAIN=8                 # seconds to let each pipeline catch up after the last frame
RX_LAT=40.0392          # Willard Airport (the generator's default receiver)
RX_LON=-88.2781
OUT=/out
WORK=/work
mkdir -p "$OUT" "$WORK"
git config --global --add safe.directory /repo 2>/dev/null

log() { printf '[%(%H:%M:%S)T] %s\n' -1 "$*"; }

# ---------------------------------------------------------------------------
# One-time setup: build the new pipeline's tools, stage the Fall 2025 web app.
# ---------------------------------------------------------------------------
log "building core/ (Release)"
cmake -S /repo/core -B "$WORK/core-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DADSB_BUILD_TESTS=OFF -DADSB_BUILD_BENCH=OFF >/dev/null || exit 1
cmake --build "$WORK/core-build" >/dev/null || exit 1
SYNTH="$WORK/core-build/tools/adsb-synth"
REPLAY="$WORK/core-build/tools/adsb-replay"
LIVE="$WORK/core-build/tools/adsb-live"
CLOCK="$WORK/core-build/tools/adsb-clock"
GATEWAY=/repo/gateway/bin/adsb-gateway.mjs

# The Fall 2025 code as delivered: extracted by run.sh from the git tag
# fall2025-final into bench/legacy/fall2025/, never the working tree's ads/.
FALL=/repo/bench/legacy/fall2025
[[ -f "$FALL/ads/server.js" && -f "$FALL/ads-b-receiver/adsb_streamer_optimized.py" ]] || {
  log "missing $FALL (run bench/legacy/run.sh, which extracts the fall2025-final tag)"; exit 1; }
rm -rf "$WORK/ads" && mkdir -p "$WORK/ads"
cp -R "$FALL/ads/." "$WORK/ads/"
rm -rf "$WORK/ads/node_modules"
ln -s /opt/legacy/ads/node_modules "$WORK/ads/node_modules"

{
  echo "{"
  echo "  \"readsb_commit\": \"$(cat /opt/readsb.commit)\","
  echo "  \"readsb_version\": \"$(readsb --version 2>&1 | head -1)\","
  echo "  \"python\": \"$(python3 --version 2>&1)\","
  echo "  \"node\": \"$(node --version)\","
  echo "  \"kernel\": \"$(uname -sr)\","
  echo "  \"cpus\": $(nproc),"
  echo "  \"scenarios\": \"$SCENARIOS\", \"duration_s\": $DURATION, \"seed\": $SEED,"
  echo "  \"fall2025_ref\": \"${FALL2025_REF:-unknown}\", \"fall2025_commit\": \"${FALL2025_COMMIT:-unknown}\","
  echo "  \"repo_commit\": \"$(git -C /repo rev-parse --short HEAD 2>/dev/null || echo unknown)\""
  echo "}"
} > "$OUT/environment.json"
readsb --help 2>&1 | grep -E -- '--net-ro-(size|interval)' > "$OUT/readsb_output_buffering.txt"

reset_db() {
  python3 - <<'PY'
import os, psycopg2
c = psycopg2.connect(os.environ["DATABASE_URL"], sslmode="require"); c.autocommit = True
c.cursor().execute("TRUNCATE aircraft_real, aircraft_history_real, aircraft_sessions_real, aircraft_position_history")
PY
}

PIDS=()
stop_all() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done
  PIDS=()
}
trap stop_all EXIT

wait_for_port() {  # host port timeout_s
  for _ in $(seq 1 $(( $3 * 10 ))); do nc -z "$1" "$2" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}

# ---------------------------------------------------------------------------
for N in $SCENARIOS; do
  D="$OUT/n$N"
  rm -rf "$D" && mkdir -p "$D"
  log "=== scenario: $N aircraft, ${DURATION}s"

  "$SYNTH" -o "$D/traffic.adsbrec" --aircraft "$N" --duration "$DURATION" --seed "$SEED" \
           --lines 1 --truth "$D/truth.csv" > "$D/synth.txt"

  reset_db

  # 1. readsb in place of the RTL-SDR + dump1090 on the Pi (default output buffering).
  readsb --net-only --net-bi-port 30004 --net-sbs-port 30003 --lat "$RX_LAT" --lon "$RX_LON" \
         --quiet > "$D/readsb.log" 2>&1 &
  PIDS+=($!)
  wait_for_port 127.0.0.1 30003 20 || { log "readsb did not start"; exit 1; }

  # 2. Tap readsb's SBS output (also used for the decoder comparison).
  python3 /repo/bench/legacy/probe.py sbs --out "$D/sbs.csv" &
  PIDS+=($!)

  # 3. The Fall 2025 streamer, unmodified, with its production flags.
  python3 "$FALL/ads-b-receiver/adsb_streamer_optimized.py" --host 127.0.0.1 --port 30003 \
          --receiver-id bench --batch-size 100 --flush-interval 1.0 --cleanup-interval 5 \
          > "$D/streamer.log" 2>&1 &
  PIDS+=($!)

  # 4. The Fall 2025 web server, unmodified.
  (cd "$WORK/ads" && PORT=3000 exec node server.js > "$D/server.log" 2>&1) &
  PIDS+=($!)
  wait_for_port 127.0.0.1 3000 30 || { log "web server did not start"; tail -20 "$D/server.log"; exit 1; }

  # 5. Probes: what the database and the map's API show, and CPU use.
  python3 /repo/bench/legacy/probe.py db  --out "$D/db.csv"  --interval 0.1 &
  PIDS+=($!)
  python3 /repo/bench/legacy/probe.py api --out "$D/api.csv" --snapshot-out "$D/api_snapshots.csv" \
          --interval 0.1 --snapshot-every 1.0 &
  PIDS+=($!)
  python3 /repo/bench/legacy/probe.py cpu --out "$D/cpu.csv" \
          --names adsb_streamer_optimized.py server.js readsb &
  PIDS+=($!)
  sleep 1

  # 6. Feed the recording in real time, then let the pipeline drain.
  python3 /repo/bench/legacy/feeder.py "$D/traffic.adsbrec" --meta "$D/feed.json" | tee "$D/feeder.txt"
  sleep "$DRAIN"
  stop_all

  # 7. AFTER: the new pipeline end to end, on the same recording, fed again
  #    in real time to a fresh readsb, alone on the machine:
  #    readsb --Beast:30005--> adsb-live (decode + track state + publisher)
  #    --multicast feed--> adsb-gateway --WebSocket--> map (probe.py ws)
  log "new pipeline: readsb -> adsb-live -> multicast feed -> gateway -> websocket"
  readsb --net-only --net-bi-port 30004 --net-bo-port 30005 --lat "$RX_LAT" --lon "$RX_LON" \
         --quiet > "$D/new_readsb.log" 2>&1 &
  PIDS+=($!)
  wait_for_port 127.0.0.1 30005 20 || { log "readsb did not start"; exit 1; }
  node "$GATEWAY" --feed 239.19.0.1:41900 --interface 127.0.0.1 --retransmit 127.0.0.1:41901 \
       --snapshot 127.0.0.1:41902 --listen 127.0.0.1:8080 --clock-helper "$CLOCK" --quiet \
       > "$D/new_gateway.txt" 2>&1 &
  GW_PID=$!
  PIDS+=($GW_PID)
  wait_for_port 127.0.0.1 8080 20 || { log "gateway did not start"; cat "$D/new_gateway.txt"; exit 1; }
  "$LIVE" --port 30005 --receiver "$RX_LAT,$RX_LON" --stats-every 0 --quiet \
          --publish 239.19.0.1:41900 --publish-interface 127.0.0.1 > "$D/new_live.txt" 2>&1 &
  LIVE_PID=$!
  PIDS+=($LIVE_PID)
  python3 /repo/bench/legacy/probe.py ws --ws-port 8080 --out "$D/new_ws.csv" \
          --snapshot-out "$D/new_ws_snapshots.csv" --snapshot-every 1.0 &
  PIDS+=($!)
  # The Fall 2025 map's own API, now answered by the gateway: same probe as above.
  python3 /repo/bench/legacy/probe.py api --api-port 8080 --out "$D/new_api.csv" \
          --snapshot-out "$D/new_api_snapshots.csv" --interval 0.1 --snapshot-every 1.0 &
  PIDS+=($!)
  python3 /repo/bench/legacy/probe.py cpu --out "$D/new_cpu.csv" --names adsb-live adsb-gateway readsb &
  PIDS+=($!)
  sleep 1
  python3 /repo/bench/legacy/feeder.py "$D/traffic.adsbrec" --meta "$D/new_feed.json" | tee "$D/new_feeder.txt"
  sleep "$DRAIN"
  python3 -c "import urllib.request,sys; sys.stdout.write(urllib.request.urlopen('http://127.0.0.1:8080/gateway/stats', timeout=5).read().decode())" \
          > "$D/new_gateway_stats.json" || true
  kill -INT "$LIVE_PID" 2>/dev/null   # adsb-live prints its latency summary on Ctrl-C
  wait "$LIVE_PID" 2>/dev/null
  kill -TERM "$GW_PID" 2>/dev/null    # the gateway prints its final lines on SIGTERM
  wait "$GW_PID" 2>/dev/null
  stop_all

  # 8. The new pipeline's core alone (replay, no network), for context.
  log "new pipeline replay"
  "$REPLAY" "$D/traffic.adsbrec" --pipeline --speed 1 --busy-wait > "$D/new_pipeline.txt"
  "$REPLAY" "$D/traffic.adsbrec" --out "$D/new_updates.bin" --hash-only > /dev/null

  # 9. Analyze this scenario.
  if python3 /repo/bench/legacy/analyze.py scenario "$D" > "$D/summary.txt"; then
    cat "$D/summary.txt"
    # Raw logs are large; metrics.json and summary.txt keep the results.
    if [[ "$KEEP_RAW" != "1" ]]; then
      rm -f "$D"/{traffic.adsbrec,truth.csv,sbs.csv,db.csv,api.csv,api_snapshots.csv,new_updates.bin} \
            "$D"/{new_ws.csv,new_ws_snapshots.csv,new_api.csv,new_api_snapshots.csv}
    fi
  else
    log "analysis failed (raw logs kept in $D)"
    cat "$D/summary.txt"
  fi
done

python3 /repo/bench/legacy/analyze.py report "$OUT" > "$OUT/report.md"
log "done: $OUT/report.md"
