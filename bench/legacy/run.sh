#!/usr/bin/env bash
# Runs the Fall 2025 baseline benchmark in Docker and copies the summarized
# results to bench/results/legacy-baseline/. See bench/legacy/README.md.
#
#   bench/legacy/run.sh                       # default sweep: 50 200 1000 3000 aircraft, 120 s each
#   SCENARIOS="50 200" DURATION=60 bench/legacy/run.sh   # quicker run
#
# The Fall 2025 code is taken from the git tag `fall2025-final` (override with
# FALL2025_REF), never from the working tree: ads/, ads-b-receiver/ and
# data-generator/ may be edited later, and the "before" numbers must keep
# measuring the system as it was delivered.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# RESULTS_DIR: where the summary goes (bench/lab/run.sh points it at its run).
RESULTS="${RESULTS_DIR:-$ROOT/bench/results/legacy-baseline}"
cd "$HERE"
REF="${FALL2025_REF:-fall2025-final}"
# Extract the original code into fall2025/ (gitignored, recreated every run).
FALL_COMMIT="$("$HERE/extract-fall2025.sh" "$REF")"
echo "Fall 2025 code: $REF = $(git -C "$ROOT" rev-parse --short "$FALL_COMMIT")"
rm -rf out && mkdir -p out
cleanup() { docker compose down -v --remove-orphans >/dev/null 2>&1 || true; }
trap cleanup EXIT
docker compose build --quiet
docker compose run --rm \
  -e SCENARIOS="${SCENARIOS:-50 200 1000 3000}" \
  -e DURATION="${DURATION:-120}" \
  -e SEED="${SEED:-2025}" \
  -e KEEP_RAW="${KEEP_RAW:-0}" \
  -e FALL2025_REF="$REF" \
  -e FALL2025_COMMIT="$FALL_COMMIT" \
  bench
mkdir -p "$RESULTS"
cp out/report.md out/environment.json "$RESULTS/"
for d in out/n*/; do
  n="$(basename "$d")"
  mkdir -p "$RESULTS/$n"
  cp "$d/metrics.json" "$d/summary.txt" "$d/new_pipeline.txt" "$RESULTS/$n/" 2>/dev/null || true
done
echo "results: $RESULTS/report.md (raw logs in $HERE/out/)"
