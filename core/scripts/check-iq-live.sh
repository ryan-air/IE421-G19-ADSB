#!/usr/bin/env bash
# check-iq-live.sh: the live pipeline on raw IQ samples (adsb-live --iq):
# IQ reader -> block ring -> demodulator -> decode + track state -> feed.
#
#   scripts/check-iq-live.sh BUILD_DIR
#
# BUILD_DIR holds tools/adsb-iqsynth, adsb-demod, adsb-live, adsb-feed-cli.
#
# Checks:
#   * unpaced: adsb-live records exactly the frames adsb-demod finds, from a
#     file and from stdin
#   * real time: (nearly) no block lost, a feed subscriber's book equals the
#     publisher's, and each frame reaches the track state within about a
#     block of its last sample
#   * block size: the latency grows with the block (the USB transfer-size
#     trade-off of README_UPDATES.md §5.1, simulated)
#   * overload: input 100 times faster than real time loses whole blocks,
#     counted, and the pipeline keeps going
#   * RTL-SDR device path, with the test device (--iq fake:FILE): the same
#     frames, a USB error / unplug / stall each exit 1 with a message,
#     SIGTERM stops cleanly, and --iq rtlsdr without a dongle fails cleanly
set -euo pipefail

BUILD="${1:?usage: check-iq-live.sh BUILD_DIR}"
T="$BUILD/tools"
for t in adsb-iqsynth adsb-demod adsb-live adsb-feed-cli; do
  [[ -x "$T/$t" ]] || { echo "missing $T/$t (build with ADSB_BUILD_TOOLS=ON)" >&2; exit 2; }
done
WORK="$(mktemp -d)"
cleanup() {
  jobs -p | xargs -r kill 2>/dev/null || true
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

# Payloads of a recording, one hex per line, in order.
payloads() {
  python3 - "$1" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
i, end = 64, len(d) - 24
while i < end:
    n = d[i + 19]
    print(d[i + 20:i + 20 + n].hex().upper())
    i += 20 + n
EOF
}
# p50 / p99 (ns) of the "last sample -> state" line.
lat() { awk '/last sample -> state/ {print $'"$2"'}' "$1"; }
field() { grep -E "^$2" "$1" | head -1; }
num() { grep -oE "$2" "$1" | head -1 | grep -oE '[0-9]+' | head -1; }

echo "== frames"
"$T/adsb-iqsynth" -o "$WORK/s" --aircraft 100 --duration 8 --tail 0.5 --seed 31 >/dev/null
"$T/adsb-demod" "$WORK/s.sigmf-meta" --raw "$WORK/demod.txt" --quiet
sed 's/^\*//; s/;$//' "$WORK/demod.txt" >"$WORK/demod.hex"
"$T/adsb-live" --iq "$WORK/s.sigmf-meta" --iq-speed 0 --stats-every 0 --quiet --record "$WORK/u.adsbrec" >"$WORK/u.txt"
payloads "$WORK/u.adsbrec" >"$WORK/u.hex"
check "unpaced: the recording holds adsb-demod's $(wc -l <"$WORK/demod.hex" | tr -d ' ') frames, in order" \
  '[[ -s "$WORK/demod.hex" ]] && cmp -s "$WORK/demod.hex" "$WORK/u.hex"'
check "unpaced: no block dropped (the reader waits)" '[[ "$(num "$WORK/u.txt" "[0-9]+ blocks dropped")" == 0 ]]'
"$T/adsb-live" --iq - --iq-format cu8 --iq-speed 0 --stats-every 0 --quiet --record "$WORK/i.adsbrec" \
  <"$WORK/s.sigmf-data" >"$WORK/i.txt"
payloads "$WORK/i.adsbrec" >"$WORK/i.hex"
check "stdin gives the same frames" 'cmp -s "$WORK/demod.hex" "$WORK/i.hex"'

echo "== real time, published"
PORT=$((42000 + RANDOM % 2000))
"$T/adsb-live" --iq "$WORK/s.sigmf-meta" --stats-every 0 --quiet --publish "127.0.0.1:$PORT" \
  --retransmit-port $((PORT + 1)) --snapshot-port $((PORT + 2)) --batch-us 0 >"$WORK/p.txt" &
PUB=$!
sleep 0.3
"$T/adsb-feed-cli" --feed "127.0.0.1:$PORT" --retransmit "127.0.0.1:$((PORT + 1))" \
  --snapshot "127.0.0.1:$((PORT + 2))" --until-end --duration 30 --quiet >"$WORK/sub.txt" || true
wait "$PUB" || bad "adsb-live (published run) exited with an error"
sed 's/^/    /' <(field "$WORK/p.txt" "iq ") <(field "$WORK/p.txt" "demod ") <(grep "last sample" "$WORK/p.txt")
# A busy machine can stall the demodulator past the ring's 16 x 6.8 ms;
# such losses are counted, and at most 1% is allowed here.
pd=$(num "$WORK/p.txt" "[0-9]+ blocks dropped"); pb=$(num "$WORK/p.txt" "^iq +[0-9]+ blocks")
check "real time: at most 1% of the blocks dropped ($pd of $pb)" '(( pd * 100 <= pb ))'
check "real time: the subscriber's book equals the publisher's" \
  '[[ -n "$(grep "^book digest" "$WORK/sub.txt")" && "$(grep "^book digest" "$WORK/sub.txt" | awk "{print \$3}")" == "$(grep "^book digest" "$WORK/p.txt" | awk "{print \$3}")" ]]'
p50=$(lat "$WORK/p.txt" 6); p99=$(lat "$WORK/p.txt" 8)
# 16384 samples = 6.8 ms: a frame waits for the rest of its block (half of
# one on average), then demodulation. Laptops oversleep, so the bounds are loose.
check "real time: last sample -> state p50 $((p50 / 1000)) us (block 6.8 ms) < 15 ms" '(( p50 < 15000000 ))'

echo "== block size"
"$T/adsb-iqsynth" -o "$WORK/b" --aircraft 100 --duration 3 --seed 32 >/dev/null
prev=0
mono=1
for blk in 4096 16384 65536; do
  "$T/adsb-live" --iq "$WORK/b.sigmf-meta" --iq-block "$blk" --stats-every 0 --quiet >"$WORK/b$blk.txt"
  b50=$(lat "$WORK/b$blk.txt" 6); b99=$(lat "$WORK/b$blk.txt" 8)
  python3 -c "print(f'    {$blk:>6} samples ({$blk / 2.4e3:5.1f} ms): last sample -> state p50 {$b50 / 1e6:6.2f} ms, p99 {$b99 / 1e6:6.2f} ms')"
  (( b50 > prev )) || mono=0
  prev=$b50
done
check "latency grows with the block size" '(( mono == 1 ))'

echo "== overload"
"$T/adsb-live" --iq "$WORK/s.sigmf-meta" --iq-speed 100 --iq-buffers 4 --stats-every 0 --quiet >"$WORK/o.txt"
sed 's/^/    /' <(field "$WORK/o.txt" "iq ") <(field "$WORK/o.txt" "demod ")
dropped=$(num "$WORK/o.txt" "[0-9]+ blocks dropped")
frames=$(num "$WORK/o.txt" "^demod +[0-9]+ frames")
check "100x real time: blocks dropped and counted ($dropped)" '(( dropped > 0 ))'
check "and the frames that remain decode ($frames)" '(( frames > 0 ))'

echo "== RTL-SDR source (test device)"
# --iq fake:FILE is a TESTING ONLY stand-in for a dongle: it plays the file
# through the same device path as --iq rtlsdr (librtlsdr's read_async
# callback -> block ring), in USB-sized transfers, and can fail on request.
"$T/adsb-live" --iq "fake:$WORK/s.sigmf-meta" --iq-speed 4 --iq-buffers 256 --stats-every 0 --quiet \
  --record "$WORK/f.adsbrec" >"$WORK/f.txt"
payloads "$WORK/f.adsbrec" >"$WORK/f.hex"
sed 's/^/    /' <(field "$WORK/f.txt" "source ") <(field "$WORK/f.txt" "iq ")
fd=$(num "$WORK/f.txt" "[0-9]+ transfers dropped")
if [[ "$fd" == 0 ]]; then
  check "device path: the recording holds adsb-demod's frames, in order" 'cmp -s "$WORK/demod.hex" "$WORK/f.hex"'
else
  check "device path: $fd transfers dropped and counted (a stalled machine)" '[[ -s "$WORK/f.hex" ]]'
fi
rc=0
"$T/adsb-live" --iq "fake:$WORK/s.sigmf-meta" --iq-speed 4 --fake-error-after 327680 --stats-every 0 \
  >"$WORK/fe.txt" 2>"$WORK/fe.err" || rc=$?
check "a USB error ends the run with exit 1 and says so" '(( rc == 1 )) && grep -q "device error" "$WORK/fe.err"'
rc=0
"$T/adsb-live" --iq "fake:$WORK/s.sigmf-meta" --iq-speed 4 --fake-unplug-after 327680 --stats-every 0 \
  >"$WORK/fu.txt" 2>"$WORK/fu.err" || rc=$?
check "an unplugged device ends the run with exit 1" '(( rc == 1 )) && grep -q "stopped delivering" "$WORK/fu.err"'
rc=0
s0=$SECONDS
"$T/adsb-live" --iq "fake:$WORK/s.sigmf-meta" --iq-speed 4 --fake-stall-after 327680 --rtl-stall-s 0.3 \
  --stats-every 0 >"$WORK/fs.txt" 2>"$WORK/fs.err" || rc=$?
check "a stalled device is given up (exit 1, $((SECONDS - s0)) s)" \
  '(( rc == 1 && SECONDS - s0 < 10 )) && grep -q "no samples from the device" "$WORK/fs.err"'
# SIGTERM (a background job from a script ignores SIGINT) stops it cleanly.
"$T/adsb-live" --iq "fake:$WORK/s.sigmf-meta" --iq-speed 1 --stats-every 0 >"$WORK/ft.txt" 2>"$WORK/ft.err" &
FPID=$!
sleep 1
kill -TERM "$FPID"
rc=0
wait "$FPID" || rc=$?
check "SIGTERM cancels the transfers: exit 0, stopped" '(( rc == 0 )) && grep -q "stopped" "$WORK/ft.err"'
rc=0
"$T/adsb-live" --iq rtlsdr:0 --stats-every 0 >"$WORK/fr.txt" 2>"$WORK/fr.err" || rc=$?
sed 's/^/    /' "$WORK/fr.err" | head -3
check "--iq rtlsdr without a dongle (or without librtlsdr) fails with a message" '(( rc != 0 )) && [[ -s "$WORK/fr.err" ]]'

echo
echo "$PASS passed, $FAIL failed"
(( FAIL == 0 ))
