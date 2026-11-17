#!/usr/bin/env bash
# Extracts the delivered Fall 2025 code (ads/, ads-b-receiver/) from the git
# tag fall2025-final into bench/legacy/fall2025/, which the bench image and
# the baseline harness use instead of the working tree. Called by
# bench/legacy/run.sh and bench/live/run.sh (both build that image).
#
#   bench/legacy/extract-fall2025.sh [REF]     # default: $FALL2025_REF or fall2025-final
#
# Prints the full commit hash it extracted. If the tag is missing (a fresh
# or shallow clone, such as CI's), it is fetched from the first remote that
# has it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
REF="${1:-${FALL2025_REF:-fall2025-final}}"

if ! git -C "$ROOT" rev-parse --verify --quiet "$REF^{commit}" > /dev/null; then
  depth=()
  [[ "$(git -C "$ROOT" rev-parse --is-shallow-repository)" == true ]] && depth=(--depth=1)
  for remote in $(git -C "$ROOT" remote); do
    git -C "$ROOT" fetch --quiet "${depth[@]}" "$remote" "refs/tags/$REF:refs/tags/$REF" 2>/dev/null && break
  done
fi
COMMIT="$(git -C "$ROOT" rev-parse --verify --quiet "$REF^{commit}")" || {
  echo "extract-fall2025: git ref $REF not found locally or on any remote (try: git fetch --tags)" >&2
  exit 2
}
rm -rf "$HERE/fall2025" && mkdir -p "$HERE/fall2025"
git -C "$ROOT" archive "$COMMIT" ads ads-b-receiver | tar -x -C "$HERE/fall2025"
echo "$COMMIT"
