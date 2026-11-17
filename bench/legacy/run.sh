#!/usr/bin/env bash
# Runs the Fall 2025 baseline benchmark in Docker and copies the summarized
# results to bench/results/legacy-baseline/. See bench/legacy/README.md.
#
#   bench/legacy/run.sh                       # default sweep: 50 200 1000 3000 aircraft, 120 s each
#   SCENARIOS="50 200" DURATION=60 bench/legacy/run.sh   # quicker run
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
RESULTS="$ROOT/bench/results/legacy-baseline"
cd "$HERE"
rm -rf out && mkdir -p out
cleanup() { docker compose down -v --remove-orphans >/dev/null 2>&1 || true; }
trap cleanup EXIT
docker compose build --quiet
docker compose run --rm \
  -e SCENARIOS="${SCENARIOS:-50 200 1000 3000}" \
  -e DURATION="${DURATION:-120}" \
  -e SEED="${SEED:-2025}" \
  bench
mkdir -p "$RESULTS"
cp out/report.md out/environment.json "$RESULTS/"
for d in out/n*/; do
  n="$(basename "$d")"
  mkdir -p "$RESULTS/$n"
  cp "$d/metrics.json" "$d/summary.txt" "$RESULTS/$n/" 2>/dev/null || true
done
echo "results: $RESULTS/report.md (raw logs in $HERE/out/)"
