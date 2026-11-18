#!/usr/bin/env bash
# Overload test (README_UPDATES.md §10): drive adsb-live past its saturation
# rate and check that it degrades the way §10.1 defines.
#
#   core/scripts/check-overload.sh BUILD_DIR
#
# The state stage is slowed to a known cost (--simulate-work-ns 10000, about
# 90k frames/s), so the multiples of saturation are reproducible on any host.
# adsb-beast-serve plays a recording as a Beast TCP stream at a fixed rate,
# like readsb's output; it blocks if the consumer stops reading.
#
# Checks, at 0.5x, 2x and 10x saturation and through a 10x burst:
#   * loss happens only at the input boundary, and is counted
#     (no frame is lost anywhere else: the feed's subscriber reconstructs the
#     publisher's book byte for byte, and every frame is either processed or
#     counted as dropped);
#   * the loss matches what the stage can absorb (about 1 - saturation/rate);
#   * queueing delay stays bounded by --max-queue, at every load;
#   * the upstream sender is never pushed back (it stays on schedule);
#   * after a burst, dropping stops promptly;
#   * memory does not grow with load; nothing crashes.
# And, for contrast, the old policy (--overload block) at 10x: no counted loss,
# but unbounded queueing delay and a sender that falls far behind, i.e. the
# loss moves upstream, uncounted.
#
# Environment: OVL_PORT (base port, default 43800). KEEP=1 keeps the work dir.

set -uo pipefail
# Empty arrays are expanded as ${a[@]+"${a[@]}"}: macOS ships bash 3.2, where
# "${a[@]}" on an empty array is an unbound-variable error under set -u.
BUILD="${1:?usage: check-overload.sh BUILD_DIR}"
T="$BUILD/tools"
for t in adsb-synth adsb-live adsb-beast-serve adsb-feed-cli; do
  [[ -x "$T/$t" ]] || { echo "missing $T/$t (build with tools enabled)"; exit 2; }
done
PORT="${OVL_PORT:-43800}"
W="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; [[ -n "${KEEP:-}" ]] && echo "kept $W" || rm -rf "$W"' EXIT
FAIL=0
check() {  # description, condition
  if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1   [$2]"; FAIL=1; fi
}
num() { grep -E "$2" "$1" | head -1 | sed -E "s/$3/\1/"; }
WORK_NS=10000
MAXQ=2048
"$T/adsb-synth" -o "$W/rec.adsbrec" --aircraft 1000 --duration 30 --seed 5 > /dev/null || exit 2

# run NAME RATE DURATION [extra adsb-beast-serve args] -- [extra adsb-live args]
run() {
  local name=$1 rate=$2 dur=$3; shift 3
  local serve_args=() live_args=()
  while [[ $# -gt 0 && $1 != -- ]]; do serve_args+=("$1"); shift; done
  [[ $# -gt 0 ]] && shift
  live_args=("$@")
  local P=$((PORT)) F=$((PORT + 1))
  "$T/adsb-feed-cli" --feed "127.0.0.1:$F" --retransmit "127.0.0.1:$((F + 1))" \
      --snapshot "127.0.0.1:$((F + 2))" --until-end --duration $((dur + 20)) --quiet > "$W/$name.sub" 2>&1 &
  local SUB=$!
  "$T/adsb-beast-serve" "$W/rec.adsbrec" --port "$P" --rate "$rate" --loop --duration "$dur" \
      ${serve_args[@]+"${serve_args[@]}"} > "$W/$name.serve" 2> "$W/$name.serve.err" &
  local SV=$!
  sleep 0.3
  "$T/adsb-live" --port "$P" --quiet --stats-every 0.5 --simulate-work-ns "$WORK_NS" \
      --publish "127.0.0.1:$F" --retransmit-port $((F + 1)) --snapshot-port $((F + 2)) \
      ${live_args[@]+"${live_args[@]}"} > "$W/$name.live" 2>&1 &
  local LV=$!
  local rss_max=0
  while kill -0 "$SV" 2>/dev/null; do
    local rss
    rss=$(ps -o rss= -p "$LV" 2>/dev/null | tr -d ' ')
    [[ -n "$rss" && "$rss" -gt "$rss_max" ]] && rss_max=$rss
    sleep 0.2
  done
  wait "$SV"
  sleep 0.5
  kill -INT "$LV"; wait "$LV"; echo "exit $?" >> "$W/$name.live"
  wait "$SUB"; echo "exit $?" >> "$W/$name.sub"
  echo "$rss_max" > "$W/$name.rss"
  PORT=$((PORT + 10))
}

late_ms()   { num "$W/$1.serve" '^lateness' '.* max ([0-9.]+) ms.*'; }
dropped()   { num "$W/$1.live" '^overload' '.*, ([0-9]+) frames dropped.*'; }
drop_pct()  { num "$W/$1.live" '^overload' '.*\(([0-9.]+)%\).*'; }
qwait_ms()  { grep -E 'queue wait' "$W/$1.live" | awk '{printf "%.1f", $10 / 1e6}'; }
received()  { num "$W/$1.live" '^stream' '.*bytes, ([0-9]+) frames,.*'; }
processed() { num "$W/$1.live" '^engine' '^engine +([0-9]+) frames.*'; }
digest()    { grep -E '^book digest' "$W/$1.$2" | awk '{print $3}'; }
exit_of()   { grep -E '^exit' "$W/$1.$2" | awk '{print $2}'; }
rss_mb()    { awk '{printf "%d", $1 / 1024}' "$W/$1.rss"; }

echo "== overload: --simulate-work-ns $WORK_NS (saturation about 90k frames/s), --max-queue $MAXQ =="
run half 45000 6 -- --max-queue "$MAXQ"
run x2 180000 6 -- --max-queue "$MAXQ"
run x10 900000 6 -- --max-queue "$MAXQ"
# Queueing bound: MAXQ frames at about WORK_NS each, with generous slack for a
# shared CI host (the old blocking policy waits ~700 ms here: unbounded by MAXQ).
BOUND_MS=$(( MAXQ * WORK_NS / 1000000 * 5 ))

for n in half x2 x10; do
  check "$n: adsb-live and its subscriber exited cleanly" '[[ "$(exit_of '$n' live)" == 0 && "$(exit_of '$n' sub)" == 0 ]]'
  check "$n: every frame received was processed or counted as dropped ($(received $n) = $(processed $n) + $(dropped $n))" \
        '(( $(received '$n') == $(processed '$n') + $(dropped '$n') ))'
  check "$n: the feed stayed consistent (subscriber book == publisher book)" \
        '[[ -n "$(digest '$n' live)" && "$(digest '$n' live)" == "$(digest '$n' sub)" ]]'
  check "$n: queue wait bounded ($(qwait_ms $n) ms max, bound $BOUND_MS ms)" '(( $(qwait_ms '$n' | cut -d. -f1) <= BOUND_MS ))'
  check "$n: the sender was never pushed back ($(late_ms $n) ms max lateness)" '(( $(late_ms '$n' | cut -d. -f1) < 250 ))'
done
check "0.5x: no frames dropped ($(dropped half))" '(( $(dropped half) == 0 ))'
check "2x: about half dropped ($(drop_pct x2)%)" '(( $(drop_pct x2 | cut -d. -f1) >= 30 && $(drop_pct x2 | cut -d. -f1) <= 70 ))'
check "10x: about 90% dropped ($(drop_pct x10)%)" '(( $(drop_pct x10 | cut -d. -f1) >= 80 ))'
check "memory does not grow with load ($(rss_mb half) MB at 0.5x, $(rss_mb x10) MB at 10x)" \
      '(( $(rss_mb x10) <= $(rss_mb half) + 32 ))'

echo "== burst: 0.5x, with 10x for 2 s from t = 2 s =="
run burst 45000 8 --burst 900000:2:2 -- --max-queue "$MAXQ"
# Drops per 0.5 s stats interval: they must start in the burst and stop soon after it.
grep -E '^\[' "$W/burst.live" | sed -E 's/^\[ *([0-9.]+) s\].* ([0-9]+) dropped.*/\1 \2/' > "$W/burst.drops"
last_drop_t=$(awk 'NR>1 && $2 > prev {t=$1} {prev=$2} END {print t+0}' "$W/burst.drops")
check "burst: frames were dropped during the burst ($(dropped burst))" '(( $(dropped burst) > 0 ))'
check "burst: dropping stopped within 1.5 s of the burst ending (last new drop at ${last_drop_t} s; burst ends at about 4.3 s)" \
      'awk "BEGIN {exit !($last_drop_t <= 5.8)}"'
check "burst: the feed stayed consistent" '[[ "$(digest burst live)" == "$(digest burst sub)" ]]'

echo "== contrast: the old policy (--overload block) at 10x =="
# 10x, not 2x: at 2x the excess (~2 MB/s) can hide in the kernel's TCP buffers
# (several MB each side on Linux) for a whole short run, so the sender is not
# visibly pushed back on every host. The queueing delay check does not depend
# on buffer sizes at all.
run block 900000 6 -- --overload block
check "block: no loss is counted ($(dropped block))" '(( $(dropped block) == 0 ))'
check "block: queueing delay is unbounded instead ($(qwait_ms block) ms max, vs the $BOUND_MS ms bound with drop)" \
      '(( $(qwait_ms block | cut -d. -f1) > BOUND_MS ))'
check "block: the sender was pushed back ($(late_ms block) ms behind schedule)" '(( $(late_ms block | cut -d. -f1) > 500 ))'

if (( FAIL )); then
  for f in "$W"/*.live "$W"/*.serve "$W"/*.sub; do echo "--- $f"; cat "$f"; done
fi
exit "$FAIL"
