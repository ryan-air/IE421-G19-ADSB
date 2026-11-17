#!/usr/bin/env bash
# Runs the live-ingest integration test (bench/live/smoke.sh) in the bench
# Docker image, which contains readsb. Output: bench/live/out/summary.txt.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
docker build --quiet -f "$ROOT/bench/legacy/Dockerfile" -t legacy-bench:latest "$ROOT" >/dev/null
rm -rf "$HERE/out" && mkdir -p "$HERE/out"
docker run --rm -v "$ROOT":/repo:ro -v "$HERE/out":/out legacy-bench:latest bash /repo/bench/live/smoke.sh
