#!/usr/bin/env bash
# Runs inside the `bench` container (see docker-compose.yml). For each fleet
# size it generates synthetic traffic, plays it through the unmodified Fall 2025
# pipeline in real time, probes what the database and the map's API show, then
# replays the same recording through the new C++ pipeline for comparison.
#
#   synthetic Beast --> readsb --SBS-1:30003--> Fall 2025 streamer --> Postgres
#                                                                   --> Node server --> /api/aircraft
#
# Environment:
#   SCENARIOS  fleet sizes to run         (default "50 200 1000 3000")
#   DURATION   seconds of traffic per run (default 120)
#   SEED       synthetic traffic seed     (default 2025)

set -uo pipefail

SCENARIOS="${SCENARIOS:-50 200 1000 3000}"
DURATION="${DURATION:-120}"
SEED="${SEED:-2025}"
DRAIN=8                 # seconds to let the old pipeline catch up after the last frame
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

rm -rf "$WORK/ads" && mkdir -p "$WORK/ads"
cp -R /repo/ads/. "$WORK/ads/"
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
  python3 /repo/ads-b-receiver/adsb_streamer_optimized.py --host 127.0.0.1 --port 30003 \
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

  # 7. The new pipeline on the same recording, at the same real-time rate.
  log "new pipeline replay"
  "$REPLAY" "$D/traffic.adsbrec" --pipeline --speed 1 --busy-wait > "$D/new_pipeline.txt"
  "$REPLAY" "$D/traffic.adsbrec" --out "$D/new_updates.bin" --hash-only > /dev/null

  # 8. Analyze this scenario.
  python3 /repo/bench/legacy/analyze.py scenario "$D" > "$D/summary.txt" || { log "analysis failed"; cat "$D/summary.txt"; }
  cat "$D/summary.txt"
done

python3 /repo/bench/legacy/analyze.py report "$OUT" > "$OUT/report.md"
log "done: $OUT/report.md"
