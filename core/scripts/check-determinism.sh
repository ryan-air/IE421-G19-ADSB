#!/usr/bin/env bash
# check-determinism.sh — verify the replay determinism contract
# (README_UPDATES.md §11.2): one recording + one build => byte-identical output
# regardless of replay speed or threading. Any further build directories (the
# naive baseline, another compiler) must produce exactly the same output too.
#
# Usage:
#   scripts/check-determinism.sh BUILD_DIR [OTHER_BUILD_DIR ...]
#
# Each directory must contain tools/adsb-replay (BUILD_DIR also tools/adsb-synth).
# Exits nonzero on any mismatch.

set -euo pipefail

BUILD="${1:?usage: check-determinism.sh BUILD_DIR [OTHER_BUILD_DIR ...]}"
shift
OTHERS=("$@")
SYNTH="$BUILD/tools/adsb-synth"
REPLAY="$BUILD/tools/adsb-replay"
for t in "$SYNTH" "$REPLAY"; do
  [[ -x "$t" ]] || { echo "missing $t (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
REC="$WORK/check.adsbrec"

# Two receiver lines and elevated corruption exercise duplicates, A/P
# filtering, and bad-CRC handling, not just the happy path.
"$SYNTH" -o "$REC" --aircraft 120 --duration 60 --lines 2 --corrupt 0.01 --seed 421 >/dev/null

declare -a NAMES HASHES
run() {
  local name="$1"; shift
  local h
  h="$("$@" --hash-only)"
  [[ -n "$h" ]] || { echo "FAIL: '$name' produced no hash" >&2; exit 1; }
  NAMES+=("$name")
  HASHES+=("$h")
  printf '  %-28s %s\n' "$name" "$h"
}

echo "replay hashes:"
run "inline, max speed"        "$REPLAY" "$REC"
run "inline, max speed (again)" "$REPLAY" "$REC"
run "inline, 200x paced"       "$REPLAY" "$REC" --speed 200
run "pipeline, max speed"      "$REPLAY" "$REC" --pipeline
run "pipeline, 200x paced"     "$REPLAY" "$REC" --pipeline --speed 200
for other in ${OTHERS[@]+"${OTHERS[@]}"}; do
  [[ -x "$other/tools/adsb-replay" ]] || { echo "missing $other/tools/adsb-replay" >&2; exit 2; }
  label="$(basename "$other")"
  run "$label, inline"   "$other/tools/adsb-replay" "$REC"
  run "$label, pipeline" "$other/tools/adsb-replay" "$REC" --pipeline
done

for i in "${!HASHES[@]}"; do
  if [[ "${HASHES[$i]}" != "${HASHES[0]}" ]]; then
    echo "FAIL: '${NAMES[$i]}' differs from '${NAMES[0]}'" >&2
    exit 1
  fi
done
echo "OK: all ${#HASHES[@]} replays produced identical output"
