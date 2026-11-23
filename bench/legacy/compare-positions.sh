#!/usr/bin/env bash
# compare-positions.sh: position-by-position comparison of the new decoder
# with readsb on the same synthetic frames (see compare_positions.py).
#
#   bench/legacy/compare-positions.sh [AIRCRAFT] [SECONDS] [SEED]    # default 300 90 5
#
# Runs in the benchmark image (readsb v3.16.21): the recording is fed to
# readsb in real time (SBS output tapped, as in the baseline benchmark) and
# replayed through adsb-replay --out. Results in bench/legacy/out/positions-nN/.
set -euo pipefail
N="${1:-300}"; DUR="${2:-90}"; SEED="${3:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [[ "${IN_BENCH_IMAGE:-0}" != 1 ]]; then
  ROOT="$(cd "$HERE/../.." && pwd)"
  OUT="$HERE/out/positions-n$N"
  rm -rf "$OUT" && mkdir -p "$OUT"
  "$HERE/extract-fall2025.sh" "${FALL2025_REF:-fall2025-final}" >/dev/null  # the image build needs it
  (cd "$HERE" && docker compose build --quiet bench)
  image="$(cd "$HERE" && docker compose config --images | grep -E -- "-bench$" | head -1)"  # (lists every service)
  docker run --rm -e IN_BENCH_IMAGE=1 -v "$ROOT":/repo:ro -v "$OUT":/w --entrypoint bash "$image" \
    /repo/bench/legacy/compare-positions.sh "$N" "$DUR" "$SEED"
  python3 "$HERE/compare_positions.py" "$OUT" | tee "$OUT/summary.txt"
  exit 0
fi

# --- inside the image --------------------------------------------------------
cmake -S /repo/core -B /tmp/core-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DADSB_BUILD_TESTS=OFF -DADSB_BUILD_BENCH=OFF >/dev/null
cmake --build /tmp/core-build --target adsb-synth adsb-replay >/dev/null
T=/tmp/core-build/tools
"$T/adsb-synth" -o /w/t.adsbrec --aircraft "$N" --duration "$DUR" --seed "$SEED" --truth /w/truth.csv
"$T/adsb-replay" /w/t.adsbrec --out /w/new.bin --hash-only >/dev/null

# The receiver position the generator uses by default (Willard Airport), as in run_bench.sh.
readsb --net-only --net-bi-port 30004 --net-sbs-port 30003 --lat 40.0392 --lon -88.2781 --quiet >/w/readsb.log 2>&1 &
RSB=$!
for _ in $(seq 1 100); do nc -z 127.0.0.1 30003 2>/dev/null && break; sleep 0.1; done
python3 /repo/bench/legacy/probe.py sbs --out /w/sbs.csv &
PROBE=$!
sleep 0.5
python3 /repo/bench/legacy/feeder.py /w/t.adsbrec --meta /w/feed.json
sleep 3   # readsb flushes its output every 50 ms; let the last positions arrive
kill "$PROBE" "$RSB" 2>/dev/null || true
wait 2>/dev/null || true
