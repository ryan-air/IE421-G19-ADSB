#!/usr/bin/env bash
# check-beast-import.sh: end-to-end checks of adsb-beast-import on a Beast
# stream written by adsb-synth (whose frames are known).
#
#   scripts/check-beast-import.sh BUILD_DIR
#
#   * every frame is imported, from a file and from stdin alike
#   * the header carries the receiver position, source and the wall time of
#     the window's first frame (--start plus the time since the input's first
#     frame, with the zone offset applied)
#   * --from/--hours keep exactly the frames in the window
#   * the imported recording replays: the generated aircraft are tracked
#   * two captures back to back (the decoder restarted) import as one
#     recording with one clock reset and time still moving forward
#   * an empty window, a --start without a zone and an impossible --receiver
#     are refused
set -euo pipefail

BUILD="${1:?usage: check-beast-import.sh BUILD_DIR}"
SYNTH="$BUILD/tools/adsb-synth"
IMPORT="$BUILD/tools/adsb-beast-import"
REPLAY="$BUILD/tools/adsb-replay"
for t in "$SYNTH" "$IMPORT" "$REPLAY"; do
  [[ -x "$t" ]] || { echo "missing $t (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { echo "PASS $*"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

# Reads an .adsbrec: prints a JSON object with the header fields and every
# record's rx_ns.
cat > "$WORK/rec.py" <<'PYEOF'
import json, struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:8] == b"ADSBREC1"
start, lat, lon, alt = struct.unpack_from("<Qiii", d, 16)
source = d[40:56].rstrip(b"\0").decode()
flags = struct.unpack_from("<I", d, 56)[0]
rx, i, end = [], 64, len(d) - 24
while i < end:
    rx.append(struct.unpack_from("<Q", d, i)[0])
    i += 20 + d[i + 19]
print(json.dumps({"start": start, "lat": lat, "lon": lon, "alt": alt, "source": source,
                  "has_pos": flags & 1, "rx": rx}))
PYEOF
field() { python3 -c "import json,sys; v=json.load(open(sys.argv[1]))[sys.argv[2]]; print(v)" "$1" "$2"; }

"$SYNTH" -o "$WORK/s.adsbrec" --aircraft 60 --duration 60 --seed 4 --receiver 41.878,-87.647 \
  --beast "$WORK/s.beast" > "$WORK/synth.txt"
want=$(sed -n 's/.*: \([0-9]*\) records.*/\1/p' "$WORK/synth.txt")

START=2026-11-19T09:36:00-06:00
START_NS=$(python3 -c "import datetime as d; print(int(d.datetime(2026,11,19,15,36,tzinfo=d.timezone.utc).timestamp())*10**9)")
"$IMPORT" "$WORK/s.beast" -o "$WORK/all.adsbrec" --receiver 41.878,-87.647,180 --source dump1090-fa \
  --start "$START" > "$WORK/all.txt"
"$IMPORT" - -o "$WORK/stdin.adsbrec" --receiver 41.878,-87.647,180 --source dump1090-fa \
  --start "$START" < "$WORK/s.beast" > /dev/null
python3 "$WORK/rec.py" "$WORK/all.adsbrec" > "$WORK/all.json"
got=$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))['rx']))" "$WORK/all.json")
if [[ "$got" == "$want" && "$want" -gt 1000 ]]; then
  ok "all $want frames imported"
else
  bad "imported $got of $want frames"; cat "$WORK/all.txt"
fi
if cmp -s "$WORK/all.adsbrec" "$WORK/stdin.adsbrec"; then
  ok "stdin input gives the same recording"
else
  bad "stdin and file inputs differ"
fi
if [[ "$(field "$WORK/all.json" lat),$(field "$WORK/all.json" lon),$(field "$WORK/all.json" alt)" == \
      "418780000,-876470000,180" && "$(field "$WORK/all.json" has_pos)" == 1 &&
      "$(field "$WORK/all.json" source)" == dump1090-fa && "$(field "$WORK/all.json" start)" == "$START_NS" ]]; then
  ok "header: receiver position, source and start time (zone applied)"
else
  bad "header fields wrong"; head -c 300 "$WORK/all.json"; echo
fi

# Window 20 s .. 30 s after the first frame.
"$IMPORT" "$WORK/s.beast" -o "$WORK/win.adsbrec" --from "$(python3 -c 'print(20/3600)')" \
  --hours "$(python3 -c 'print(10/3600)')" --start "$START" > "$WORK/win.txt"
python3 "$WORK/rec.py" "$WORK/win.adsbrec" > "$WORK/win.json"
if python3 - "$WORK/all.json" "$WORK/win.json" "$START_NS" <<'PYEOF'
import json, sys
a = json.load(open(sys.argv[1])); w = json.load(open(sys.argv[2])); start = int(sys.argv[3])
t0 = a["rx"][0]
want = [r for r in a["rx"] if 20e9 <= r - t0 < 30e9]
assert w["rx"] == want, (len(w["rx"]), len(want))
assert w["start"] == start + (want[0] - t0), (w["start"], start, want[0] - t0)
PYEOF
then
  ok "--from/--hours keep exactly the window; its header time is its first frame's"
else
  bad "window selection or its start time is wrong"
fi

"$REPLAY" "$WORK/all.adsbrec" > "$WORK/replay.txt"
created=$(sed -n 's/^tracks *created \([0-9]*\).*/\1/p' "$WORK/replay.txt")
if [[ -n "$created" && "$created" -ge 55 && "$created" -le 70 ]]; then
  ok "the imported recording replays: $created tracks for 60 aircraft"
else
  bad "replay tracked ${created:-?} aircraft, want ~60"; cat "$WORK/replay.txt"
fi

# The same capture twice: the second copy's counter starts a minute earlier.
cat "$WORK/s.beast" "$WORK/s.beast" > "$WORK/twice.beast"
"$IMPORT" "$WORK/twice.beast" -o "$WORK/twice.adsbrec" > "$WORK/twice.txt"
python3 "$WORK/rec.py" "$WORK/twice.adsbrec" > "$WORK/twice.json"
if grep -q "resets 1," "$WORK/twice.txt" && python3 - "$WORK/twice.json" "$want" <<'PYEOF'
import json, sys
rx = json.load(open(sys.argv[1]))["rx"]
assert len(rx) == 2 * int(sys.argv[2])
assert all(b >= a for a, b in zip(rx, rx[1:]))
PYEOF
then
  ok "a restarted decoder: one reset, every frame, time never goes back"
else
  bad "restart handling"; cat "$WORK/twice.txt"
fi

if "$IMPORT" "$WORK/s.beast" -o "$WORK/empty.adsbrec" --from 1 > /dev/null 2>&1 || [[ -e "$WORK/empty.adsbrec" ]]; then
  bad "an empty window is accepted"
else
  ok "an empty window is refused and writes no file"
fi
if "$IMPORT" "$WORK/s.beast" -o "$WORK/x.adsbrec" --start 2026-11-19T09:36:00 > /dev/null 2>&1; then
  bad "a --start without a zone is accepted"
else
  ok "a --start without a zone is refused"
fi
if "$IMPORT" "$WORK/s.beast" -o "$WORK/x.adsbrec" --receiver 95,0 > /dev/null 2>&1; then
  bad "an impossible --receiver is accepted"
else
  ok "an impossible --receiver is refused"
fi

echo "passed $PASS, failed $FAIL"
[[ $FAIL -eq 0 ]]
