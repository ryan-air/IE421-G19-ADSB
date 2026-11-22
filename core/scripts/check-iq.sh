#!/usr/bin/env bash
# check-iq.sh: end-to-end checks of the synthetic IQ generator
# (adsb-iqsynth, README_UPDATES.md §22.4), with readsb as an independent
# decoder.
#
#   scripts/check-iq.sh BUILD_DIR [READSB]
#
# BUILD_DIR holds tools/adsb-iqsynth. READSB (or $READSB, or readsb on PATH)
# is a readsb binary; it only needs the ifile input (build with RTLSDR=no).
#
# Checks:
#   * output does not depend on the thread count; every format has the
#     expected size; the SigMF metadata is valid JSON with one annotation per
#     rendered message
#   * readsb decodes the cu8 (UC8) and cs16 (SC16) files: clean messages at
#     15-30 dB SNR at >= 97% (stronger ones clip at the default gain), no
#     false DF17 and few other false decodes, every timestamp within one sample of the truth (readsb's
#     Beast output, matched by hex and time)
#   * receiver clock error (ppm), DC offset and IQ imbalance do not break
#     decoding; strong multipath costs decodes, and readsb also decodes some
#     echoes (the same message, microseconds later)
#   * the gain trade-off: high gain loses the nearby ground traffic to
#     clipping, low gain decodes it
set -euo pipefail

BUILD="${1:?usage: check-iq.sh BUILD_DIR [READSB]}"
SYNTH="$BUILD/tools/adsb-iqsynth"
READSB="${2:-${READSB:-$(command -v readsb || true)}}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SCORE="$HERE/iq-score.py"
[[ -x "$SYNTH" ]] || { echo "missing $SYNTH (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
[[ -n "$READSB" && -x "$READSB" ]] || { echo "need readsb: pass its path or set READSB" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { echo "PASS $*"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

# readsb's Beast output with sample-clock timestamps, via a TCP connector to
# a one-shot listener (readsb connects to it, sends, exits at end of file).
decode() {  # decode DATA FORMAT OUT
  local data="$1" fmt="$2" out="$3" portfile="$WORK/port"
  rm -f "$portfile"
  python3 - "$out" "$portfile" <<'EOF' &
import socket, sys
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen(1)
s.settimeout(60)
open(sys.argv[2] + ".tmp", "w").write(str(s.getsockname()[1]))
import os
os.rename(sys.argv[2] + ".tmp", sys.argv[2])
c, _ = s.accept()
c.settimeout(60)
with open(sys.argv[1], "wb") as f:
    while True:
        b = c.recv(65536)
        if not b:
            break
        f.write(b)
EOF
  local listener=$!
  for _ in $(seq 100); do [[ -f "$portfile" ]] && break; sleep 0.05; done
  "$READSB" --device-type ifile --ifile "$data" --iformat "$fmt" --no-fix --net \
    --net-connector "127.0.0.1,$(cat "$portfile"),beast_out" --quiet >"$out.log" 2>&1 || true
  wait "$listener"
}

echo "== determinism and formats"
"$SYNTH" -o "$WORK/t1" --aircraft 60 --duration 2 --threads 1 --seed 7 >/dev/null
"$SYNTH" -o "$WORK/t4" --aircraft 60 --duration 2 --threads 4 --seed 7 >/dev/null
if cmp -s "$WORK/t1.sigmf-data" "$WORK/t4.sigmf-data" && cmp -s "$WORK/t1.sigmf-meta" "$WORK/t4.sigmf-meta"; then
  ok "1 and 4 threads give identical files"
else
  bad "output depends on the thread count"
fi
"$SYNTH" --data - --no-meta --aircraft 60 --duration 2 --seed 7 2>/dev/null >"$WORK/stdout.data"
cmp -s "$WORK/t1.sigmf-data" "$WORK/stdout.data" && ok "--data - streams the same bytes" || bad "--data - differs"
samples=$(python3 -c "import json;print(json.load(open('$WORK/t1.sigmf-meta'))['global']['adsb:stats']['samples'])")
for f in cu8:2 cs8:2 cs16:4 cf32:8; do
  name="${f%%:*}"; bps="${f##*:}"
  "$SYNTH" --data "$WORK/f.$name" --no-meta --format "$name" --aircraft 60 --duration 2 --seed 7 >/dev/null
  size=$(wc -c <"$WORK/f.$name" | tr -d ' ')
  [[ "$size" == $((samples * bps)) ]] && ok "$name: $size bytes = $samples samples x $bps" || bad "$name size $size"
done
if python3 - "$WORK/t1.sigmf-meta" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
g = m["global"]
assert g["core:datatype"] == "cu8" and g["core:version"] == "1.0.0" and g["core:sample_rate"] == 2.4e6
assert m["captures"][0]["core:frequency"] == 1090e6
a = m["annotations"]
assert len(a) == g["adsb:stats"]["rendered"] > 0
assert all(x["core:sample_start"] <= x["adsb:sample_pos"] < x["core:sample_start"] + x["core:sample_count"] for x in a)
assert [x["core:sample_start"] for x in a] == sorted(x["core:sample_start"] for x in a)
assert len(g["adsb:transmitters"]) == 60
EOF
then ok "SigMF metadata valid"; else bad "SigMF metadata"; fi

echo "== readsb decodes the files"
# readsb's own false decodes (DF11/16/20/21 whose parity noise satisfies,
# about 1 per second here) are allowed up to ~0.5% of the decodes; a false
# DF17 never: its plain CRC would mean the generator sent wrong bits.
REQ=(--require "clean>=15<30:0.97" --require "false<=30" --require "false17<=0" --require "timing<=1.0")
"$SYNTH" -o "$WORK/a" --aircraft 150 --duration 10 --tail 1 --seed 11 >"$WORK/a.log"
decode "$WORK/a.sigmf-data" UC8 "$WORK/a.beast"
if python3 "$SCORE" "$WORK/a.sigmf-meta" "$WORK/a.beast" "${REQ[@]}" >"$WORK/a.score"; then
  ok "cu8: $(head -1 "$WORK/a.score")"
else
  bad "cu8"; cat "$WORK/a.score"
fi
sed 's/^/    /' "$WORK/a.score"

"$SYNTH" -o "$WORK/b" --format cs16 --aircraft 150 --duration 10 --tail 1 --seed 11 >/dev/null
decode "$WORK/b.sigmf-data" SC16 "$WORK/b.beast"
python3 "$SCORE" "$WORK/b.sigmf-meta" "$WORK/b.beast" "${REQ[@]}" >"$WORK/b.score" &&
  ok "cs16: $(head -1 "$WORK/b.score")" || { bad "cs16"; cat "$WORK/b.score"; }

echo "== receiver impairments and multipath"
"$SYNTH" -o "$WORK/c" --aircraft 100 --duration 6 --tail 1 --seed 12 --ppm 40 --dc 0.01,-0.01 \
  --iq-imbalance 0.05,3 >/dev/null
decode "$WORK/c.sigmf-data" UC8 "$WORK/c.beast"
python3 "$SCORE" "$WORK/c.sigmf-meta" "$WORK/c.beast" "${REQ[@]}" >"$WORK/c.score" &&
  ok "40 ppm, DC offset, IQ imbalance: $(head -1 "$WORK/c.score")" || { bad "impairments"; cat "$WORK/c.score"; }
"$SYNTH" -o "$WORK/d" --aircraft 100 --duration 6 --tail 1 --seed 12 --ground-reflection 0.7 \
  --reflector 40.06,-88.25,250,3 >/dev/null
decode "$WORK/d.sigmf-data" UC8 "$WORK/d.beast"
python3 "$SCORE" "$WORK/d.sigmf-meta" "$WORK/d.beast" --json >"$WORK/d.json"
python3 "$SCORE" "$WORK/d.sigmf-meta" "$WORK/d.beast" | sed 's/^/    /'
if python3 - "$WORK/d.json" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
t = s["timing"]
print(f"    {s['false']} false decodes, {t['late_copies']} of them late copies of real messages")
sys.exit(0 if s["false"] > 0 and t["late_copies"] >= 0.8 * s["false"] else 1)
EOF
then ok "readsb decodes echoes: the false decodes are the real messages, delayed"; else bad "echo decodes"; fi
python3 "$SCORE" "$WORK/a.sigmf-meta" "$WORK/a.beast" --json >"$WORK/a.json"
if python3 - "$WORK/a.json" "$WORK/d.json" <<'EOF'
import json, sys
def clean_rate(p):
    s = json.load(open(p))
    k = sum(b["clean"][0] for b in s["bands"] if 15 <= b["snr_lo"] < 30)
    n = sum(b["clean"][1] for b in s["bands"] if 15 <= b["snr_lo"] < 30)
    return k / n
a, d = clean_rate(sys.argv[1]), clean_rate(sys.argv[2])
print(f"    clean 15-30 dB: {100*a:.1f}% without strong multipath, {100*d:.1f}% with it")
sys.exit(0 if d < a else 1)
EOF
then ok "multipath costs decodes"; else bad "multipath had no effect"; fi

echo "== gain trade-off (nearby ground traffic)"
near() {  # decode rate of messages within 5 km
  python3 - "$1" "$2" <<'EOF'
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("s", os.environ["SCORE"])
s = importlib.util.module_from_spec(spec); spec.loader.exec_module(s)
meta, truth, dec, _ = s.load(sys.argv[1], sys.argv[2])
s.score_by_time(truth, dec, 12e6 / meta["global"]["core:sample_rate"])
near = [a for a in truth if a["adsb:range_m"] < 5000 and not a["adsb:collided"]]
print(f"{sum(a['_hit'] for a in near)} {len(near)}")
EOF
}
export SCORE
"$SYNTH" -o "$WORK/g1" --aircraft 60 --ground 0.3 --duration 6 --tail 1 --seed 13 --full-scale -70 >/dev/null
"$SYNTH" -o "$WORK/g2" --aircraft 60 --ground 0.3 --duration 6 --tail 1 --seed 13 --full-scale -35 >/dev/null
decode "$WORK/g1.sigmf-data" UC8 "$WORK/g1.beast"
decode "$WORK/g2.sigmf-data" UC8 "$WORK/g2.beast"
read -r h1 n1 <<<"$(near "$WORK/g1.sigmf-meta" "$WORK/g1.beast")"
read -r h2 n2 <<<"$(near "$WORK/g2.sigmf-meta" "$WORK/g2.beast")"
echo "    within 5 km: high gain $h1/$n1, low gain $h2/$n2 decoded"
if (( n1 > 20 && h2 * 10 >= n2 * 9 && h1 * 2 < h2 )); then
  ok "high gain clips nearby traffic, low gain decodes it"
else
  bad "gain trade-off not visible"
fi

echo
echo "$PASS passed, $FAIL failed"
(( FAIL == 0 ))
