#!/usr/bin/env bash
# check-ab.sh: A/B line arbitration (README_UPDATES.md §6) end to end.
#
#   scripts/check-ab.sh BUILD_DIR
#
# BUILD_DIR holds tools/adsb-synth, adsb-replay, adsb-beast-serve, adsb-live,
# adsb-iqsynth and adsb-demod.
#
# Checks:
#   * replay (adsb-replay --arbitrate) of two simulated receivers: the output is
#     the union of the lines, the engine sees no cross-line duplicate, the
#     relative radio clock comes out at the generator's +18 ppm, inline and
#     two-thread replay decide alike, and an outage of one line loses no
#     aircraft
#   * live, two Beast servers (one per line) into adsb-live: replaying the
#     run's recording repeats its arbitration exactly; when line B stops,
#     line A carries on and no track is lost
#   * live, two IQ receivers with independent noise (our demodulator on each
#     line): the arbiter matches exactly the frames both demodulators decoded
#     (counted apart from it, by payload and sample time), and recovers line
#     B's +20 ppm sample clock
set -euo pipefail

BUILD="${1:?usage: check-ab.sh BUILD_DIR}"
T="$BUILD/tools"
for t in adsb-synth adsb-replay adsb-beast-serve adsb-live adsb-iqsynth adsb-demod; do
  [[ -x "$T/$t" ]] || { echo "missing $T/$t (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
done
WORK="$(mktemp -d)"
cleanup() {
  jobs -p | xargs kill 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT
PASS=0
FAIL=0
ok() { echo "PASS $*"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }
check() {  # check NAME CONDITION
  if eval "$2"; then ok "$1"; else bad "$1"; fi
}
# field FILE PREFIX WORD: the number after WORD on the first line starting with PREFIX.
field() {
  awk -v p="$2" -v w="$3" 'index($0, p) == 1 {
    for (i = 1; i < NF; i++) { f = $i; sub(/,$/, "", f); if (f == w) { v = $(i + 1); sub(/,$/, "", v); print v; found = 1; exit } }
  } END { if (!found) print 0 }' "$1"
}
# before FILE PREFIX WORD: the number before WORD (as in "12020 frames").
before() {
  awk -v p="$2" -v w="$3" 'index($0, p) == 1 {
    for (i = 2; i <= NF; i++) { f = $i; sub(/,$/, "", f); if (f == w) { print $(i - 1); found = 1; exit } }
  } END { if (!found) print 0 }' "$1"
}
arb() { grep -E '^arb' "$1" || true; }
# Run with a time limit (macOS has no timeout(1)).
limit() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }
PORT_A=$((33000 + RANDOM % 2000))
PORT_B=$((PORT_A + 1))

# ---- Replay ------------------------------------------------------------------
"$T/adsb-synth" -o "$WORK/ab.adsbrec" --aircraft 200 --duration 30 --lines 2 --seed 7 >/dev/null
"$T/adsb-replay" "$WORK/ab.adsbrec" --arbitrate > "$WORK/r1.txt"
"$T/adsb-replay" "$WORK/ab.adsbrec" --arbitrate --pipeline > "$WORK/r2.txt"
fwd=$(field "$WORK/r1.txt" "arbiter" forwarded)
multi=$(field "$WORK/r1.txt" "arbiter" multi)
rx0=$(field "$WORK/r1.txt" "arb line 0" received)
rx1=$(field "$WORK/r1.txt" "arb line 1" received)
echo "replay: line 0 $rx0, line 1 $rx1, forwarded $fwd, both $multi"
check "replay: the output is larger than either line alone" "[[ $fwd -gt $rx0 && $fwd -gt $rx1 ]]"
check "replay: forwarded + dropped = offered" \
  "[[ $((fwd + $(field "$WORK/r1.txt" arbiter dropped))) -eq $(field "$WORK/r1.txt" arbiter offered) ]]"
check "replay: the engine sees no duplicate" "grep -q 'duplicates 0,' '$WORK/r1.txt'"
check "replay: relative clock +18.000 ppm" "grep -q 'rate +18.000 ppm' '$WORK/r1.txt'"
check "replay: inline and pipeline decide alike" \
  "[[ \"\$(arb '$WORK/r1.txt')\" == \"\$(arb '$WORK/r2.txt')\" ]] && \
   [[ \"\$(grep '^output hash' '$WORK/r1.txt')\" == \"\$(grep '^output hash' '$WORK/r2.txt')\" ]]"
"$T/adsb-replay" "$WORK/ab.adsbrec" --arb-window-us 100 > "$WORK/r3.txt"
check "replay: a 100 us window matches fewer copies (line B is ~250 us later)" \
  "[[ $(field "$WORK/r3.txt" arbiter multi) -lt $((multi / 2)) ]]"

# Outage: line B unplugged from 5 to 20 s. Line A alone still keeps every
# aircraft (none expires); the output keeps every frame line A got.
"$T/adsb-synth" -o "$WORK/out.adsbrec" --aircraft 200 --duration 30 --lines 2 --seed 7 --outage 1:5:20 > "$WORK/s.txt"
"$T/adsb-replay" "$WORK/out.adsbrec" --arbitrate --no-flush > "$WORK/r4.txt"
"$T/adsb-replay" "$WORK/ab.adsbrec" --arbitrate --no-flush > "$WORK/r5.txt"
check "outage: the generator dropped line B's frames" "grep -qE 'outage [1-9][0-9]{3,}' '$WORK/s.txt'"
check "outage: line A's frames are unchanged" \
  "[[ $(field "$WORK/r4.txt" "arb line 0" received) -eq $(field "$WORK/r5.txt" "arb line 0" received) ]]"
check "outage: no track expires, as without it" \
  "[[ \"\$(grep '^tracks' '$WORK/r4.txt' | sed 's/, live.*//')\" == \"\$(grep '^tracks' '$WORK/r5.txt' | sed 's/, live.*//')\" ]] && grep -q 'expired 0,' '$WORK/r4.txt'"

# ---- Live, Beast ---------------------------------------------------------------
"$T/adsb-synth" -o "$WORK/live.adsbrec" --aircraft 200 --duration 18 --lines 2 --seed 9 >/dev/null
# Time-limited, so a run where adsb-live never connects cannot hang the script.
limit 40 "$T/adsb-beast-serve" "$WORK/live.adsbrec" --port "$PORT_A" --line 0 --speed 3 > "$WORK/sa.txt" 2>&1 &
limit 40 "$T/adsb-beast-serve" "$WORK/live.adsbrec" --port "$PORT_B" --line 1 --speed 3 --duration 2.5 > "$WORK/sb.txt" 2>&1 &
sleep 0.3
limit 40 "$T/adsb-live" --port "$PORT_A" --b-port "$PORT_B" --duration 7 --stats-every 1 \
  --record "$WORK/rec.adsbrec" > "$WORK/l.txt" 2> "$WORK/l.err" || true
wait || true
# Same window as the live run (it prints it).
"$T/adsb-replay" "$WORK/rec.adsbrec" --arb-window-us "$(field "$WORK/l.txt" arbiter window)" > "$WORK/lr.txt"
echo "live:"; arb "$WORK/l.txt"
check "live: both lines connected" "grep -q 'line A .* connected' '$WORK/l.err' && grep -q 'line B .* connected' '$WORK/l.err'"
check "live: replaying the recording repeats the arbitration exactly" \
  "[[ -n \"\$(arb '$WORK/l.txt')\" && \"\$(arb '$WORK/l.txt')\" == \"\$(arb '$WORK/lr.txt')\" ]] || \
   { diff <(arb '$WORK/l.txt') <(arb '$WORK/lr.txt'); false; }"
check "live: frames from both lines were matched" "[[ $(field "$WORK/l.txt" arbiter multi) -gt 1000 ]]"
check "live: relative clock +18.000 ppm from the Beast timestamps" "grep -q 'rate +18.000 ppm' '$WORK/l.txt'"
check "live: line B stopped (its server ended after 2.5 s)" "grep -q 'line B .* disconnected' '$WORK/l.err'"
la=$(field "$WORK/l.txt" "arb line 0" received)
sa=$(before "$WORK/l.txt" "stream (line A)" frames)
check "live: line A carried on alone: every frame it streamed was offered" "[[ $la -eq $sa && $la -gt $(( $(field "$WORK/l.txt" "arb line 1" received) * 2 )) ]]"
check "live: no track lost when line B stopped" "grep -qE '^tracks .*expired 0, live 200' '$WORK/l.txt'"
check "live: no frame dropped at the input boundary" "grep -q 'line A 0 dropped' '$WORK/l.txt' && grep -q 'line B 0 dropped' '$WORK/l.txt'"

# ---- Live, IQ ----------------------------------------------------------------------
# One traffic scenario, two receivers: independent noise, weak signals (so the
# lines lose different frames), line B's sample clock 20 ppm fast.
"$T/adsb-iqsynth" -o "$WORK/a" --aircraft 150 --duration 4 --seed 5 --iq-seed 100 --noise-figure 20 >/dev/null 2>&1
"$T/adsb-iqsynth" -o "$WORK/b" --aircraft 150 --duration 4 --seed 5 --iq-seed 200 --noise-figure 20 --ppm 20 >/dev/null 2>&1
limit 60 "$T/adsb-live" --iq "$WORK/a.sigmf-meta" --iq-b "$WORK/b.sigmf-meta" --stats-every 0 --quiet > "$WORK/q.txt" 2>&1 || true
"$T/adsb-demod" "$WORK/a.sigmf-meta" --beast "$WORK/a.beast" --quiet
"$T/adsb-demod" "$WORK/b.sigmf-meta" --beast "$WORK/b.beast" --quiet
# Frames both demodulators decoded: same payload, sample times within 1 ms
# (line B's 12 MHz counter runs 20 ppm fast).
cat > "$WORK/both.py" <<'PYEOF'
import sys
from collections import defaultdict
def frames(path):
    b = open(path, 'rb').read()
    # Split on unescaped 0x1A, then unescape each message.
    out, msg, i = [], None, 0
    while i < len(b):
        c = b[i]
        if c == 0x1A:
            if i + 1 < len(b) and b[i + 1] == 0x1A and msg is not None:
                msg.append(0x1A); i += 2; continue
            if msg: out.append(bytes(msg))
            msg = bytearray(); i += 1; continue
        if msg is not None: msg.append(c)
        i += 1
    if msg: out.append(bytes(msg))
    res = []
    for m in out:
        n = {0x31: 2, 0x32: 7, 0x33: 14}.get(m[0])
        if n and len(m) == 1 + 6 + 1 + n:
            res.append((int.from_bytes(m[1:7], 'big'), m[8:]))
    return res
a, b = frames(sys.argv[1]), frames(sys.argv[2])
idx = defaultdict(list)
for ts, p in b: idx[p].append(ts / (1 + 20e-6))
both = sum(1 for ts, p in a if any(abs(t - ts) < 12000 for t in idx[p]))
print(both, len(a) + len(b) - both)
PYEOF
read -r both union <<< "$(python3 "$WORK/both.py" "$WORK/a.beast" "$WORK/b.beast")"
echo "iq: both demodulators $both, union $union"; arb "$WORK/q.txt"
check "iq: matched copies = frames both demodulators decoded" "[[ $both -gt 100 && $(field "$WORK/q.txt" arbiter multi) -eq $both ]]"
check "iq: forwarded = the union" "[[ $(field "$WORK/q.txt" arbiter forwarded) -eq $union ]]"
check "iq: line B's sample clock +20.000 ppm" "grep -q 'rate +20.000 ppm' '$WORK/q.txt'"
check "live: line B on line A's own source is refused" \
  "! '$T/adsb-live' --port '$PORT_A' --b-port '$PORT_A' --duration 1 --quiet >/dev/null 2>&1"
check "iq: unpaced A/B is refused" \
  "! '$T/adsb-live' --iq '$WORK/a.sigmf-meta' --iq-b '$WORK/b.sigmf-meta' --iq-speed 0 --quiet >/dev/null 2>&1"

echo "$PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
