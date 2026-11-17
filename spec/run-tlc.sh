#!/usr/bin/env bash
# Model-check FeedRecovery.tla with TLC.
#
# Downloads a pinned, checksum-verified tla2tools.jar into spec/.tools/ if it
# is missing, then runs every configuration and compares the result with the
# expected outcome:
#   MC.cfg             correct model     -> must PASS (all invariants + liveness)
#   MC_NoSnapshot.cfg  snapshot removed  -> must FAIL with a liveness violation
#   MC_Reach.cfg       reachability probe-> must FAIL invariant NeverSnapshot
# Exits nonzero if any configuration deviates from its expectation (in
# particular, any violation in the correct model).
set -euo pipefail

TLA_VERSION="v1.7.4"   # stable release of 2024-08-08 (v1.8.0 is still a prerelease)
TLA_URL="https://github.com/tlaplus/tlaplus/releases/download/${TLA_VERSION}/tla2tools.jar"
TLA_SHA256="936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"

cd "$(dirname "$0")"
TOOLS=.tools
JAR="$TOOLS/tla2tools-${TLA_VERSION}.jar"
OUT=out

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

if [[ ! -f "$JAR" ]]; then
  mkdir -p "$TOOLS"
  echo "Downloading tla2tools.jar ${TLA_VERSION} ..."
  curl -sSfL --retry 3 -o "$JAR.part" "$TLA_URL"
  mv "$JAR.part" "$JAR"
fi
actual="$(sha256 "$JAR")"
if [[ "$actual" != "$TLA_SHA256" ]]; then
  echo "ERROR: SHA-256 mismatch for $JAR" >&2
  echo "  expected $TLA_SHA256" >&2
  echo "  actual   $actual" >&2
  rm -f "$JAR"
  exit 2
fi

command -v java >/dev/null || { echo "ERROR: java not found" >&2; exit 2; }
mkdir -p "$OUT"

# TLC exit codes: 0 = no error, 12 = invariant/safety violation,
# 13 = temporal (liveness) violation, others = error.
#        config              expected-exit  text that must appear in the output
CHECKS=( "MC.cfg             0  Model checking completed. No error has been found."
         "MC_NoSnapshot.cfg  13 Temporal properties were violated."
         "MC_Reach.cfg       12 Invariant NeverSnapshot is violated." )

failures=0
for entry in "${CHECKS[@]}"; do
  read -r cfg want text <<<"$entry"
  name="${cfg%.cfg}"
  log="$OUT/$name.out"
  echo "=== TLC $cfg (expect exit $want)"
  set +e
  java -XX:+UseParallelGC -jar "$JAR" -workers auto \
       -config "$cfg" -metadir "states/$name" FeedRecovery.tla >"$log" 2>&1
  got=$?
  set -e
  grep -E "states generated|depth of the complete|^Finished in|Error:" "$log" | sed 's/^/    /' || true
  if [[ "$got" -eq "$want" ]] && grep -qF "$text" "$log"; then
    echo "    OK (exit $got)"
  else
    echo "    UNEXPECTED: exit $got, wanted $want with \"$text\" (full log: spec/$log)"
    failures=$((failures + 1))
  fi
done

if [[ $failures -ne 0 ]]; then
  echo "$failures configuration(s) did not behave as expected." >&2
  exit 1
fi
echo "All TLC checks behaved as expected."
