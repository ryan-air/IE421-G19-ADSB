#!/usr/bin/env bash
# check-demod.sh: end-to-end checks of the Mode S demodulator (adsb-demod,
# plan §5.2-5.3) on adsb-iqsynth scenarios, scored against their truth, and
# side by side with readsb when one is available.
#
#   scripts/check-demod.sh BUILD_DIR [READSB]
#
# BUILD_DIR holds tools/adsb-iqsynth and tools/adsb-demod. READSB (or
# $READSB, or readsb on PATH) is optional: without it only the absolute
# requirements are checked.
#
# Checks:
#   * stdin and file input give the same frames; every format decodes
#   * busy airspace (150 aircraft), cu8 and cs16: clean messages at 15-30 dB
#     SNR >= 98%, no false DF17, few other false decodes, every start within
#     half a sample of the truth
#   * weak signals (noise figure 22 dB): at least as many messages as readsb
#   * receiver impairments (40 ppm, DC offset, IQ imbalance) and strong
#     multipath still decode; under multipath the false decodes are echoes
#   * 2.0 and 12 MS/s recordings decode (readsb's file input is 2.4 MS/s only;
#     2.0 MS/s loses some frames to its blind sampling phase)
#   * with readsb: at least 98% of readsb's matched messages on every
#     scenario, and no more false DF17 than readsb
#   * faster than real time
set -euo pipefail

BUILD="${1:?usage: check-demod.sh BUILD_DIR [READSB]}"
SYNTH="$BUILD/tools/adsb-iqsynth"
DEMOD="$BUILD/tools/adsb-demod"
READSB="${2:-${READSB:-$(command -v readsb || true)}}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SCORE="$HERE/iq-score.py"
for t in "$SYNTH" "$DEMOD"; do
  [[ -x "$t" ]] || { echo "missing $t (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
done
if [[ -n "$READSB" && -x "$READSB" ]]; then
  echo "readsb: $READSB"
else
  READSB=""
  echo "readsb: not found, comparisons skipped"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { echo "PASS $*"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

# readsb's Beast output, as in check-iq.sh.
readsb_decode() {  # readsb_decode DATA FORMAT OUT
  local data="$1" fmt="$2" out="$3" portfile="$WORK/port"
  rm -f "$portfile"
  python3 - "$out" "$portfile" <<'EOF' &
import os, socket, sys
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen(1)
s.settimeout(60)
open(sys.argv[2] + ".tmp", "w").write(str(s.getsockname()[1]))
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

# field NAME FILE: a number from iq-score.py --json.
field() {
  python3 - "$1" "$2" <<'EOF'
import json, sys
s = json.load(open(sys.argv[2]))
key = sys.argv[1]
if key == "matched":
    print(s["timing"]["matched"] if s["timing"] else 0)
elif key == "false17":
    print(s["false_by_df"].get("DF17", 0))
elif key == "clean1530":
    k = sum(b["clean"][0] for b in s["bands"] if 15 <= b["snr_lo"] < 30)
    n = sum(b["clean"][1] for b in s["bands"] if 15 <= b["snr_lo"] < 30)
    print(f"{k / n:.4f}" if n else "nan")
else:
    print(s[key])
EOF
}

REQ=(--require "clean>=15<30:0.98" --require "false<=30" --require "false17<=0" --require "timing<=0.5")
NS=()  # throughput samples, ns/sample

# scenario NAME FORMAT READSB_FORMAT [adsb-iqsynth options...]: generate,
# demodulate, score; compare with readsb when present and the rate allows.
scenario() {
  local name="$1" fmt="$2" rfmt="$3"
  shift 3
  "$SYNTH" -o "$WORK/$name" --format "$fmt" "$@" >/dev/null
  "$DEMOD" "$WORK/$name.sigmf-meta" --beast "$WORK/$name.ours" 2>"$WORK/$name.demod.log"
  NS+=("$(grep -o '[0-9.]* ns/sample' "$WORK/$name.demod.log" | cut -d' ' -f1)")
  python3 "$SCORE" "$WORK/$name.sigmf-meta" "$WORK/$name.ours" --json >"$WORK/$name.ours.json"
  if [[ -n "$READSB" && -n "$rfmt" ]]; then
    readsb_decode "$WORK/$name.sigmf-data" "$rfmt" "$WORK/$name.readsb"
    python3 "$SCORE" "$WORK/$name.sigmf-meta" "$WORK/$name.readsb" --json >"$WORK/$name.readsb.json"
  fi
}

# compare NAME: side by side; ours must reach 98% of readsb's matched count.
compare() {
  local name="$1" o r
  o="$(field matched "$WORK/$name.ours.json")"
  if [[ -f "$WORK/$name.readsb.json" ]]; then
    r="$(field matched "$WORK/$name.readsb.json")"
    printf '    %-12s matched: ours %6s  readsb %6s   false: ours %3s  readsb %3s   clean 15-30 dB: ours %s  readsb %s\n' \
      "$name" "$o" "$r" "$(field false "$WORK/$name.ours.json")" "$(field false "$WORK/$name.readsb.json")" \
      "$(field clean1530 "$WORK/$name.ours.json")" "$(field clean1530 "$WORK/$name.readsb.json")"
    # Under multipath a more sensitive demodulator also decodes more echoes,
    # so there only the matched count is compared.
    if (( o * 100 >= r * 98 )) && { [[ -n "${ECHO_OK:-}" ]] ||
       (( $(field false17 "$WORK/$name.ours.json") <= $(field false17 "$WORK/$name.readsb.json") )); }; then
      ok "$name: >= 98% of readsb's messages, no more false DF17"
    else
      bad "$name: below readsb ($o vs $r matched)"
    fi
  fi
}

echo "== input paths and formats"
"$SYNTH" -o "$WORK/p" --aircraft 60 --duration 2 --seed 7 >/dev/null
"$DEMOD" "$WORK/p.sigmf-meta" --raw "$WORK/p.file" --quiet
"$DEMOD" - --format cu8 --rate 2.4e6 --raw "$WORK/p.stdin" --quiet <"$WORK/p.sigmf-data"
if [[ -s "$WORK/p.file" ]] && cmp -s "$WORK/p.file" "$WORK/p.stdin"; then
  ok "stdin and file give the same $(wc -l <"$WORK/p.file" | tr -d ' ') frames"
else
  bad "stdin and file differ"
fi
"$SYNTH" --data - --no-meta --aircraft 60 --duration 2 --seed 7 2>/dev/null |
  "$DEMOD" - --format cu8 --raw "$WORK/p.pipe" --quiet
cmp -s "$WORK/p.file" "$WORK/p.pipe" && ok "adsb-iqsynth | adsb-demod streams" || bad "pipe differs"
for fmt in cs8 cs16 cf32; do
  "$SYNTH" -o "$WORK/p$fmt" --format "$fmt" --aircraft 60 --duration 2 --seed 7 >/dev/null
  "$DEMOD" "$WORK/p$fmt.sigmf-meta" --beast "$WORK/p$fmt.beast" --quiet
  if python3 "$SCORE" "$WORK/p$fmt.sigmf-meta" "$WORK/p$fmt.beast" --require "clean>=15<30:0.97" --require "false17<=0" >/dev/null; then
    ok "$fmt decodes"
  else
    bad "$fmt"
  fi
done

echo "== busy airspace"
scenario busy cu8 UC8 --aircraft 150 --duration 10 --tail 1 --seed 11
if python3 "$SCORE" "$WORK/busy.sigmf-meta" "$WORK/busy.ours" "${REQ[@]}" >"$WORK/busy.score"; then
  ok "cu8: $(head -1 "$WORK/busy.score")"
else
  bad "cu8"; cat "$WORK/busy.score"
fi
sed 's/^/    /' "$WORK/busy.score"
compare busy
scenario busy16 cs16 SC16 --aircraft 150 --duration 10 --tail 1 --seed 11
python3 "$SCORE" "$WORK/busy16.sigmf-meta" "$WORK/busy16.ours" "${REQ[@]}" >"$WORK/busy16.score" &&
  ok "cs16: $(head -1 "$WORK/busy16.score")" || { bad "cs16"; cat "$WORK/busy16.score"; }
compare busy16

echo "== weak signals"
scenario weak cu8 UC8 --aircraft 150 --duration 10 --tail 1 --seed 21 --noise-figure 22
python3 "$SCORE" "$WORK/weak.sigmf-meta" "$WORK/weak.ours" | sed 's/^/    /'
if [[ -f "$WORK/weak.readsb.json" ]]; then
  o="$(field matched "$WORK/weak.ours.json")"; r="$(field matched "$WORK/weak.readsb.json")"
  echo "    weak: ours $o, readsb $r matched ($(python3 -c "print(f'{100 * ($o - $r) / $r:+.1f}%')"))"
  (( o >= r )) && ok "weak: at least readsb's sensitivity" || bad "weak: less sensitive than readsb"
  compare weak
fi
python3 "$SCORE" "$WORK/weak.sigmf-meta" "$WORK/weak.ours" --require "false17<=0" --require "false<=30" >/dev/null &&
  ok "weak: no false DF17, few false decodes" || bad "weak: false decodes"

echo "== impairments and multipath"
scenario imp cu8 UC8 --aircraft 100 --duration 6 --tail 1 --seed 12 --ppm 40 --dc 0.01,-0.01 --iq-imbalance 0.05,3
python3 "$SCORE" "$WORK/imp.sigmf-meta" "$WORK/imp.ours" "${REQ[@]}" >"$WORK/imp.score" &&
  ok "40 ppm, DC offset, IQ imbalance: $(head -1 "$WORK/imp.score")" || { bad "impairments"; cat "$WORK/imp.score"; }
compare imp
scenario mp cu8 UC8 --aircraft 100 --duration 6 --tail 1 --seed 12 --ground-reflection 0.7 --reflector 40.06,-88.25,250,3
# Echoes are real frames decoded a second time a few microseconds later
# (readsb decodes them too); everything else must still be clean.
python3 "$SCORE" "$WORK/mp.sigmf-meta" "$WORK/mp.ours" --require "all>=15:0.3" >"$WORK/mp.score" &&
  ok "strong multipath: $(head -1 "$WORK/mp.score")" || { bad "multipath"; cat "$WORK/mp.score"; }
if python3 - "$WORK/mp.ours.json" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
other = s["false"] - s["timing"]["late_copies"]
print(f"    {s['false']} false decodes, {s['timing']['late_copies']} of them echoes, {other} others")
sys.exit(0 if other <= 30 else 1)
EOF
then ok "multipath: the false decodes are echoes"; else bad "multipath: false decodes that are not echoes"; fi
ECHO_OK=1 compare mp

echo "== other sample rates"
for rate in 2.0e6 12e6; do
  scenario "r$rate" cu8 "" --aircraft 60 --duration 3 --tail 1 --seed 14 --rate "$rate"
  # 2.0 MS/s has a blind sampling phase (a half-bit is one sample; see
  # test_demod.cpp): the sequence detector recovers part of it.
  min=0.95; [[ "$rate" == 2.0e6 ]] && min=0.85
  python3 "$SCORE" "$WORK/r$rate.sigmf-meta" "$WORK/r$rate.ours" --require "clean>=15<30:$min" --require "false17<=0" >"$WORK/r$rate.score" &&
    ok "$rate S/s: $(head -1 "$WORK/r$rate.score")" || { bad "$rate S/s"; cat "$WORK/r$rate.score"; }
done

echo "== speed"
python3 - "${NS[@]}" <<'EOF'
import sys
ns = [float(x) for x in sys.argv[1:]]
print(f"    ns/sample per scenario: {', '.join(f'{x:.1f}' for x in ns)} (one thread; real time at 2.4 MS/s = 417)")
EOF
worst="$(printf '%s\n' "${NS[@]}" | sort -g | tail -1)"
python3 -c "import sys; sys.exit(0 if $worst < 417 else 1)" && ok "faster than real time (worst $worst ns/sample)" ||
  bad "slower than real time ($worst ns/sample)"

echo
echo "$PASS passed, $FAIL failed"
(( FAIL == 0 ))
