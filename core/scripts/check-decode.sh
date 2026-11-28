#!/usr/bin/env bash
# check-decode.sh: end-to-end checks of adsb-decode on a recording written by
# adsb-synth (whose aircraft are known).
#
#   scripts/check-decode.sh BUILD_DIR
#
#   * one JSON line per record, in order, each with the record's hex
#   * every generated aircraft appears, and no other address in a CRC-checked frame
#   * the positions it prints are the track engine's: as many as adsb-replay's
#     global + local fixes, and each within 20 m of the aircraft's track
#   * if pyModeS is installed (bench/crosscheck), no field differs from it
set -euo pipefail

BUILD="${1:?usage: check-decode.sh BUILD_DIR}"
SYNTH="$BUILD/tools/adsb-synth"
DECODE="$BUILD/tools/adsb-decode"
REPLAY="$BUILD/tools/adsb-replay"
for t in "$SYNTH" "$DECODE" "$REPLAY"; do
  [[ -x "$t" ]] || { echo "missing $t (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
done
HERE="$(cd "$(dirname "$0")" && pwd)"
COMPARE="$HERE/../../bench/crosscheck/pymodes_compare.py"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { echo "PASS $*"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

"$SYNTH" -o "$WORK/s.adsbrec" --aircraft 40 --duration 60 --seed 11 --receiver 41.878,-87.647 \
  --truth "$WORK/truth.csv" > "$WORK/synth.txt"
want=$(sed -n 's/.*: \([0-9]*\) records.*/\1/p' "$WORK/synth.txt")
"$DECODE" "$WORK/s.adsbrec" -o "$WORK/d.jsonl" 2> "$WORK/decode.txt"
"$REPLAY" "$WORK/s.adsbrec" > "$WORK/replay.txt"
fixes=$(sed -n 's/^positions *global \([0-9]*\), local \([0-9]*\),.*/\1 \2/p' "$WORK/replay.txt" | awk '{print $1 + $2}')

if python3 - "$WORK/d.jsonl" "$WORK/s.adsbrec" "$want" <<'PYEOF'
import json, struct, sys
lines = [json.loads(l) for l in open(sys.argv[1])]
d = open(sys.argv[2], "rb").read()
hexes, i, end = [], 64, len(d) - 24
while i < end:
    n = d[i + 19]
    hexes.append(d[i + 20:i + 20 + n].hex().upper())
    i += 20 + n
assert len(lines) == len(hexes) == int(sys.argv[3]), (len(lines), len(hexes), sys.argv[3])
assert [o["i"] for o in lines] == list(range(len(lines)))
assert [o["hex"] for o in lines] == hexes
PYEOF
then
  ok "one line per record, in order, with the record's frame"
else
  bad "lines do not match the recording's records"
fi

if python3 - "$WORK/d.jsonl" "$WORK/truth.csv" <<'PYEOF'
import csv, json, sys
truth = {r["icao"].upper() for r in csv.DictReader(open(sys.argv[2]))}
seen = {o["icao"] for o in map(json.loads, open(sys.argv[1])) if o["st"] == "ok"}
assert seen == truth, (sorted(seen - truth)[:5], sorted(truth - seen)[:5])
PYEOF
then
  ok "CRC-checked addresses are exactly the generated aircraft"
else
  bad "addresses differ from the generated aircraft"
fi

got=$(grep -c '"pos":' "$WORK/d.jsonl" || true)
if [[ -n "$fixes" && "$got" == "$fixes" && "$got" -gt 1000 ]]; then
  ok "$got positions, as many as the engine's fixes"
else
  bad "positions $got, engine fixes ${fixes:-?}"; cat "$WORK/replay.txt"
fi

if python3 - "$WORK/d.jsonl" "$WORK/truth.csv" <<'PYEOF'
import bisect, csv, json, math, sys
tracks = {}
for r in csv.DictReader(open(sys.argv[2])):
    tracks.setdefault(r["icao"].upper(), []).append((int(r["tx_ns"]), float(r["lat"]), float(r["lon"])))
for v in tracks.values():
    v.sort()
worst = 0.0
for o in map(json.loads, open(sys.argv[1])):
    if "pos" not in o:
        continue
    pts = tracks[o["icao"]]
    t = int(o["t"].replace(".", "")) if isinstance(o["t"], str) else round(o["t"] * 1e9)
    k = bisect.bisect_left(pts, (t, -1e9, -1e9))
    best = min(math.hypot((p[1] - o["pos"][0]) * 111320.0,
                          (p[2] - o["pos"][1]) * 111320.0 * math.cos(math.radians(p[1])))
               for p in pts[max(0, k - 3):k + 3])
    worst = max(worst, best)
assert worst < 20.0, worst
print(f"worst {worst:.1f} m")
PYEOF
then
  ok "every position within 20 m of the aircraft's track"
else
  bad "a position is far from its aircraft's track"
fi

if python3 -c "import pyModeS" 2> /dev/null; then
  python3 "$COMPARE" "$WORK/d.jsonl" --surface-ref 41.878,-87.647 --json "$WORK/cmp.json" > "$WORK/cmp.txt"
  if python3 -c "import json,sys; d=json.load(open(sys.argv[1])); bad={k:v['differ'] for k,v in d['fields'].items() if v.get('differ')}; assert not bad, bad" "$WORK/cmp.json"; then
    ok "no field differs from pyModeS"
  else
    bad "fields differ from pyModeS"; cat "$WORK/cmp.txt"
  fi
else
  echo "SKIP pyModeS comparison (pip install pyModeS==3.7.0)"
fi

echo "passed $PASS, failed $FAIL"
[[ $FAIL -eq 0 ]]
