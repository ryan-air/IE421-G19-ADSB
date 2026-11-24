#!/usr/bin/env bash
# check-carrier.sh: end-to-end checks of the transponder frequency-stability
# method (plan §22.5) on adsb-iqsynth scenarios, whose transmitter offsets,
# drifts, Doppler and receiver error are known.
#
#   scripts/check-carrier.sh BUILD_DIR
#
# BUILD_DIR holds tools/adsb-iqsynth and tools/adsb-carrier. Each scenario
# is generated, run through adsb-carrier (demodulate, estimate each
# message's carrier, Doppler from the decoded track) and scored by
# carrier-study.py against the truth:
#   * busy airspace at 2.4 MS/s, known receiver ppm: per-message error is
#     what the reported standard error says, Doppler within a few Hz, each
#     aircraft's offset within ~15 Hz and its drift within ~0.2 Hz/s
#   * the receiver ppm estimated from the aircraft (no reference): within
#     3 standard errors of the truth, which are ~10 ppm with 60 aircraft;
#     with transmitters within 1 kHz, within 0.3 ppm (and with a drifting
#     LO, the ppm at t = 0)
#   * offsets near the +-1 MHz tolerance still measured at 2.4 MS/s
#   * a receiver LO drifting 0.02 ppm/s (22 Hz/s): without --rx-drift every
#     aircraft's drift is off by ~22 Hz/s; with it, recovered
#   * 6 MS/s cs16 with a reflector 12 dB down and a DC offset: multipath is
#     the largest error (4x per message) but aircraft offsets hold
#   * clipping is detected before the DC offset is removed
#   * the CSV is the same with stdin input and does not depend on the run
set -euo pipefail

BUILD="${1:?usage: check-carrier.sh BUILD_DIR}"
SYNTH="$BUILD/tools/adsb-iqsynth"
CARRIER="$BUILD/tools/adsb-carrier"
HERE="$(cd "$(dirname "$0")" && pwd)"
STUDY="$HERE/carrier-study.py"
for t in "$SYNTH" "$CARRIER"; do
  [[ -x "$t" ]] || { echo "missing $t (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { echo "PASS $*"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }

# study NAME [carrier-study options...]: score NAME.csv against NAME's truth.
study() {
  local name="$1"
  shift
  python3 "$STUDY" "$WORK/$name.csv" --truth "$WORK/$name.sigmf-meta" "$@" > "$WORK/$name.study" 2>&1
}

check() {  # check DESCRIPTION NAME [study options and --require rules...]
  local desc="$1" name="$2"
  shift 2
  if study "$name" "$@"; then
    ok "$desc"
  else
    bad "$desc"
    sed 's/^/    /' "$WORK/$name.study" | grep -E "FAIL|truth|err|_z|ppm|receiver" || true
  fi
}

gen() {  # gen NAME [adsb-iqsynth options...]
  local name="$1"
  shift
  "$SYNTH" -o "$WORK/$name" --tail 1 "$@" > "$WORK/$name.synth.log" 2>&1
  "$CARRIER" "$WORK/$name.sigmf-meta" --out "$WORK/$name.csv" 2> "$WORK/$name.carrier.log"
}

echo "== busy airspace, 2.4 MS/s cu8, receiver at +12 ppm"
gen busy --aircraft 60 --duration 60 --seed 3 --ppm 12
cat "$WORK/busy.carrier.log"
check "messages: error matches the reported sigma, Doppler within a few Hz" busy --rx-ppm 12 \
  --require truth_unmatched\<=5 --require msg_z_p50\>=0.55 --require msg_z_p50\<=0.95 \
  --require msg_err_p50_hz\<=40 --require doppler_err_p50_hz\<=2 --require doppler_err_p95_hz\<=10
check "aircraft: offsets and drifts against the transmitter truth (known ppm)" busy --rx-ppm 12 \
  --require aircraft\>=45 --require offset_err_rel_p50_hz\<=15 --require offset_z_p50\<=1.0 \
  --require drift_err_p50_hz_per_s\<=0.2 --require drift_z_p50\<=1.2
check "receiver ppm estimated from the aircraft: within 3 standard errors" busy --rx-estimate mean \
  --require rx_ppm_z\<=3 --require offset_err_rel_p50_hz\<=15
if python3 "$STUDY" "$WORK/busy.csv" --rx-estimate none --types /dev/null > "$WORK/none.out" &&
   grep -q "not removed" "$WORK/none.out"; then
  ok "--rx-estimate none reports offsets relative to the receiver LO"
else
  bad "--rx-estimate none"
fi

echo "== offsets near the +-1 MHz tolerance, 2.4 MS/s"
gen wide --aircraft 40 --duration 30 --seed 8 --tx-offset-sd 500e3
check "offsets up to ~1 MHz measured" wide --rx-ppm 0 \
  --require offset_abs_max_hz\>=700000 --require offset_err_rel_p50_hz\<=25 --require msg_z_p50\<=0.95

echo "== receiver LO drifting 0.02 ppm/s (warming up), 120 s"
gen drift --aircraft 60 --duration 120 --seed 4 --ppm 12 --lo-drift 0.02
check "without --rx-drift every aircraft's drift is off by ~22 Hz/s" drift --rx-ppm 12 \
  --require drift_err_p50_hz_per_s\>=15
check "with --rx-drift 10 the receiver drift is found and removed" drift --rx-ppm 12 --rx-drift 10 \
  --require rx_lo_drift_err_ppm_per_s\<=0.0005 --require drift_err_p50_hz_per_s\<=0.15 \
  --require offset_err_rel_p50_hz\<=15 --require offset_err_mean_abs_hz\<=30 --require offset_rel_z_p50\<=1.0

echo "== transmitters within 1 kHz, receiver +50 ppm drifting 0.005 ppm/s: the ppm estimate is precise"
# With little transmitter scatter the aircraft pin the receiver's ppm down
# to ~0.1 ppm, so a wrong sign or a ppm measured at the wrong time shows.
gen tight --aircraft 50 --duration 60 --seed 6 --ppm 50 --lo-drift 0.005 --tx-offset-sd 1000
check "ppm at t = 0 estimated from the aircraft (with --rx-drift) within 0.3 ppm" tight --rx-estimate mean \
  --rx-drift 10 --require rx_ppm_err\<=0.3 --require rx_ppm_z\<=3 --require rx_lo_drift_err_ppm_per_s\<=0.0005

echo "== 6 MS/s cs16, a reflector 12 dB down, DC offset"
# Multipath is the largest error source: the echo arrives with its own
# Doppler and overlaps other bits, and the reported sigma does not include
# it (msg_z_p50 ~2.8 here). Averaging over an aircraft's messages, whose
# geometry changes, still gives offsets within ~15 Hz.
gen multi --aircraft 40 --duration 30 --seed 9 --rate 6e6 --format cs16 --dc 0.01,-0.005 \
  --reflector 40.15,-88.25,300,12
check "multipath and DC offset: aircraft offsets within ~30 Hz, drift within ~1.5 Hz/s" multi --rx-ppm 0 \
  --require aircraft\>=30 --require offset_err_rel_p50_hz\<=30 --require drift_err_p50_hz_per_s\<=1.5 \
  --require msg_err_p50_hz\<=150

echo "== clipped samples are found before the DC offset is removed"
# cs16 full scale is 0.99997; a DC offset of 0.02 moves positive clips to
# 0.98, below the clip level, if the DC is removed first.
gen clip --aircraft 30 --duration 10 --seed 2 --format cs16 --dc 0.02,0.02 --full-scale -95
"$CARRIER" "$WORK/clip.sigmf-meta" --no-dc --out "$WORK/clip-nodc.csv" --quiet
if python3 - "$WORK/clip.csv" "$WORK/clip-nodc.csv" <<'PYEOF2'
import csv, sys
a = {r["sample_pos"]: int(r["clipped"]) for r in csv.DictReader(open(sys.argv[1]))}
b = {r["sample_pos"]: int(r["clipped"]) for r in csv.DictReader(open(sys.argv[2]))}
both = [k for k in a if k in b]
ca, cb = sum(a[k] for k in both), sum(b[k] for k in both)
print(f"    {len(both)} frames: {ca} clipped samples with DC removal, {cb} without")
# None missed (a few more after the shift: values near full scale pass it)
sys.exit(0 if both and cb > 100 and cb <= ca <= 1.02 * cb else 1)
PYEOF2
then
  ok "every sample clipped without DC removal is clipped with it"
else
  bad "clipped samples missed after DC removal"
fi

echo "== input paths"
"$CARRIER" - --format cu8 --rate 2.4e6 --receiver "$(python3 -c "
import json; r = json.load(open('$WORK/wide.sigmf-meta'))['global']['adsb:receiver']
print(f\"{r['lat']},{r['lon']},{r['ground_alt_m'] + r['antenna_height_m']}\")")" \
  --out "$WORK/stdin.csv" --quiet < "$WORK/wide.sigmf-data"
"$CARRIER" "$WORK/wide.sigmf-data" --out "$WORK/again.csv" --quiet
if cmp -s "$WORK/stdin.csv" "$WORK/wide.csv" && cmp -s "$WORK/again.csv" "$WORK/wide.csv"; then
  ok "stdin with --receiver == SigMF input, and a second run is identical"
else
  bad "stdin / SigMF / rerun outputs differ"
fi
if "$CARRIER" "$WORK/wide.sigmf-meta" --receiver 95,0 > /dev/null 2>&1; then
  bad "an impossible --receiver is accepted"
else
  ok "an impossible --receiver is refused"
fi

echo "passed $PASS, failed $FAIL"
[[ $FAIL -eq 0 ]]
