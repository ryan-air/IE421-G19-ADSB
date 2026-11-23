#!/usr/bin/env bash
# Runs the live-ingest integration test (bench/live/smoke.sh) in the bench
# Docker image, which contains readsb. Output: bench/live/out/summary.txt.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
# The image's Dockerfile installs the Fall 2025 code's dependencies from the
# extracted fall2025-final tag (bench/legacy/README.md), so extract it first.
"$ROOT/bench/legacy/extract-fall2025.sh" > /dev/null
rm -rf "$HERE/out" && mkdir -p "$HERE/out"
# Build log in out/build.log (CI uploads out/), with a time limit where
# `timeout` exists (not on macOS), so a hung download fails visibly.
limit=(); command -v timeout >/dev/null && limit=(timeout "${BUILD_TIMEOUT:-900}")
echo "building legacy-bench:latest (log: bench/live/out/build.log)"
if ! ${limit[@]+"${limit[@]}"} docker build --progress=plain -f "$ROOT/bench/legacy/Dockerfile" \
       -t legacy-bench:latest "$ROOT" >"$HERE/out/build.log" 2>&1; then
  echo "docker build failed or timed out; last lines of the log:" >&2
  tail -n 60 "$HERE/out/build.log" >&2
  exit 1
fi
docker run --rm -v "$ROOT":/repo:ro -v "$HERE/out":/out legacy-bench:latest bash /repo/bench/live/smoke.sh
