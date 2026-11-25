#!/usr/bin/env bash
# bench/lab/run.sh — the whole benchmark suite on a Linux benchmark host, in
# one command, into one results directory you copy back for analysis.
# See bench/lab/README.md.
#
#   bench/lab/run.sh --dry-run                  # what would run and change; changes nothing
#   bench/lab/run.sh                            # host, build, test, core, transports
#   bench/lab/run.sh --cpus 2,3                 # pin the measuring threads to two cores
#   bench/lab/run.sh --steps xdp --tx-port enp1s0f0 --rx-port enp1s0f1
#   bench/lab/run.sh --steps legacy             # Fall 2025 vs new pipeline (Docker, ~25 min)
#   bench/lab/run.sh --steps all --tx-port ... --rx-port ...
#   bench/lab/run.sh --steps carrier --receiver 40.1,-88.2,230   # RTL-SDR, 10 min
#
# Steps (each logs to its own file; a failing step does not stop the others):
#   host        machine description: check-host.sh, CPU, kernel, NICs
#   build       release (with tests), bench (native + LTO), naive-baseline
#   test        the unit tests and the determinism check
#   core        decode + track state: microbenchmarks, replay throughput and
#               pipeline latency, overload suite, shared-memory ring
#   transports  feed UDP transports over loopback: kernel, io_uring
#   xdp         AF_XDP over a real NIC, copy and zero-copy (needs --tx-port and
#               --rx-port: two unused ports cabled to each other; sudo)
#   legacy      bench/legacy/run.sh: Fall 2025 system vs new pipeline (Docker)
#   carrier     transponder frequency study (plan 22.5): records 1090 MHz with
#               an RTL-SDR (rtl_sdr, 2.4 MS/s; or analyses --iq FILE), then
#               adsb-carrier + carrier-study.py (needs --receiver)
# Default: host build test core transports, plus xdp when ports are given.
# xdp, legacy and carrier run only when asked for.
#
# What it changes on the machine: nothing, except
#   - build directories under core/build/ (as any build does);
#   - with xdp: for the run only, the two ports get test addresses
#     (10.77.0.1, 10.77.0.2), the RX port moves into a network namespace,
#     both are set to one queue and MTU 1500 if needed; all of it is put back
#     on exit (also on Ctrl-C). It refuses ports that have an address or a
#     route, so the connection you are logged in through cannot be touched;
#   - with --tune CPUS: core/scripts/tune-host.sh --apply, reverted on exit;
#   - with legacy: Docker images and containers of the harness (removed after);
#   - with carrier: nothing; the raw recording (about 4.8 MB/s) is deleted
#     after the analysis unless --keep-iq.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CORE="$ROOT/core"
STEPS=""
OUT=""
CPUS=""
TUNE=""
TX_PORT=""
RX_PORT=""
RATE=100000
SIZE=1200
ROUNDS=5
SECONDS_PER=3
DRY=0
QUICK=0
NS=adsblab
TX_IP=10.77.0.1
RX_IP=10.77.0.2
RECEIVER=""
SDR_SECONDS=600
SDR_GAIN=30
SDR_DEVICE=0
IQ_IN=""
KEEP_IQ=0

usage() {
  sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
  cat <<EOF

Options:
  --steps LIST     comma-separated steps, or "all"
  --out DIR        results directory (default bench/lab/runs/<UTC time>)
  --cpus A,B       pin the two measuring threads (sender/receiver, reader/worker)
  --tune CPUS      run tune-host.sh --apply --cpus CPUS first, --revert at the end
  --tx-port DEV    xdp: the sending port (no address, not in use)
  --rx-port DEV    xdp: the receiving port, cabled to the sending one
  --rate N         xdp/transports: datagrams per second (default $RATE)
  --size B         xdp/transports: datagram bytes (default $SIZE)
  --rounds N       xdp/transports: rounds per transport (default $ROUNDS)
  --receiver LAT,LON,ALT_M  carrier: the antenna's position (degrees, metres above sea level)
  --sdr-seconds N  carrier: recording length (default $SDR_SECONDS; --quick: 20)
  --sdr-gain DB    carrier: RTL-SDR gain (default $SDR_GAIN; too high clips strong aircraft)
  --sdr-device N   carrier: RTL-SDR device index (default $SDR_DEVICE)
  --iq FILE        carrier: analyse this recording instead (SigMF, or raw cu8 at 2.4 MS/s)
  --keep-iq        carrier: keep the raw recording in the results (not in the .tar.gz)
  --quick          shorter runs everywhere (a check that it all works)
  --dry-run        print the plan and the machine changes, run nothing
EOF
  exit "${1:-0}"
}

while (($#)); do
  case "$1" in
    --steps) STEPS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --cpus) CPUS="$2"; shift 2 ;;
    --tune) TUNE="$2"; shift 2 ;;
    --tx-port) TX_PORT="$2"; shift 2 ;;
    --rx-port) RX_PORT="$2"; shift 2 ;;
    --rate) RATE="$2"; shift 2 ;;
    --size) SIZE="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --receiver) RECEIVER="$2"; shift 2 ;;
    --sdr-seconds) SDR_SECONDS="$2"; shift 2 ;;
    --sdr-gain) SDR_GAIN="$2"; shift 2 ;;
    --sdr-device) SDR_DEVICE="$2"; shift 2 ;;
    --iq) IQ_IN="$2"; shift 2 ;;
    --keep-iq) KEEP_IQ=1; shift ;;
    --quick) QUICK=1; shift ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) usage 0 ;;
    *) echo "unknown option: $1" >&2; usage 1 ;;
  esac
done

ALL_STEPS="host build test core transports xdp legacy carrier"
if [[ -z $STEPS ]]; then
  STEPS="host build test core transports"
  [[ -n $TX_PORT || -n $RX_PORT ]] && STEPS="$STEPS xdp"
elif [[ $STEPS == all ]]; then
  STEPS="$ALL_STEPS"
else
  STEPS="${STEPS//,/ }"
fi
for s in $STEPS; do
  [[ " $ALL_STEPS " == *" $s "* ]] || { echo "unknown step: $s (steps: $ALL_STEPS)" >&2; exit 1; }
done
want() { [[ " $STEPS " == *" $1 "* ]]; }
if want xdp && [[ -z $TX_PORT || -z $RX_PORT || $TX_PORT == "$RX_PORT" ]]; then
  echo "the xdp step needs --tx-port and --rx-port: two different, unused ports cabled to each other" >&2
  exit 1
fi
if [[ -n $CPUS && ! $CPUS =~ ^[0-9]+,[0-9]+$ ]]; then
  echo "--cpus takes two core numbers, e.g. 2,3" >&2
  exit 1
fi
if want carrier; then
  if [[ ! $RECEIVER =~ ^-?[0-9.]+,-?[0-9.]+(,-?[0-9.]+)?$ ]]; then
    echo "the carrier step needs --receiver LAT,LON[,ALT_M] (the antenna; Doppler is computed from it)" >&2
    exit 1
  fi
  if [[ -n $IQ_IN && ! -f $IQ_IN ]]; then
    echo "--iq: no such file: $IQ_IN" >&2
    exit 1
  fi
  if [[ ! $SDR_SECONDS =~ ^[0-9]+$ || $SDR_SECONDS -lt 1 || ! $SDR_DEVICE =~ ^[0-9]+$ ]]; then
    echo "--sdr-seconds and --sdr-device take whole numbers" >&2
    exit 1
  fi
fi
if [[ $(uname -s) != Linux ]]; then
  echo "this suite is for the Linux benchmark host (this is $(uname -s))" >&2
  [[ $DRY == 1 ]] || exit 1
fi
if ((QUICK)); then SECONDS_PER=1; ROUNDS=2; fi
CPU_ARGS=()
[[ -n $CPUS ]] && CPU_ARGS=(--cpus "$CPUS")

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${OUT:-$ROOT/bench/lab/runs/$STAMP}"
# The compiler: $CXX if set, else the first one found (newest first).
if [[ -z ${CXX:-} ]]; then
  for c in g++ g++-14 g++-13 g++-12 clang++ clang++-18 clang++-17 c++; do
    command -v "$c" >/dev/null 2>&1 && { CXX="$c"; break; }
  done
fi
export CXX="${CXX:-c++}"
REL="$CORE/build/release"
BENCH="$CORE/build/bench"
NAIVE="$CORE/build/naive-baseline"

# ---------------------------------------------------------------------------
# The plan (also the whole of --dry-run)
# ---------------------------------------------------------------------------
echo "results:  $OUT"
echo "commit:   $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo ' (with local changes)')"
echo "steps:    $STEPS"
echo "compiler: $CXX ($("$CXX" --version 2>/dev/null | head -1))"
[[ -n $CPUS ]] && echo "pinning:  cores $CPUS"
if [[ -n $TUNE ]]; then
  echo "tuning:   sudo core/scripts/tune-host.sh --apply --cpus $TUNE   (reverted at the end)"
fi
if want xdp; then
  cat <<EOF
xdp:      $TX_PORT ($TX_IP) -> $RX_PORT ($RX_IP, in network namespace $NS), for the run only:
            sudo ip netns add $NS; sudo ip link set $RX_PORT netns $NS
            sudo ip addr add $TX_IP/24 dev $TX_PORT; (in $NS) ip addr add $RX_IP/24 dev $RX_PORT
            both ports up, permanent neighbour entries, one combined queue, MTU 1500 if larger
          undone on exit: namespace deleted (the port returns), address and neighbour removed,
          queues, MTU and link state put back as they were
EOF
fi
want legacy && echo "legacy:   bench/legacy/run.sh (Docker; about 25 min, a few with --quick)"
if want carrier; then
  if [[ -n $IQ_IN ]]; then
    echo "carrier:  analyse $IQ_IN (receiver $RECEIVER)"
  else
    echo "carrier:  rtl_sdr -d $SDR_DEVICE -f 1090000000 -s 2400000 -g $SDR_GAIN for $((QUICK ? 20 : SDR_SECONDS)) s" \
      "(about $(( (QUICK ? 20 : SDR_SECONDS) * 48 / 10 )) MB, deleted after unless --keep-iq), receiver $RECEIVER"
  fi
fi
if ((DRY)); then
  echo "(dry run: nothing was run or changed)"
  exit 0
fi

mkdir -p "$OUT" || exit 1
SUMMARY="$OUT/SUMMARY.md"
{
  echo "# Benchmark run $STAMP"
  echo
  echo "- commit: \`$(git -C "$ROOT" rev-parse HEAD 2>/dev/null)\`$(git -C "$ROOT" diff --quiet 2>/dev/null || echo ' (with local changes)')"
  echo "- steps: $STEPS"
  echo "- compiler: $CXX ($("$CXX" --version 2>/dev/null | head -1))"
  echo "- pinning: ${CPUS:-none}; tuning: ${TUNE:-none}; quick: $QUICK"
  echo
  echo "| step | result | log |"
  echo "|---|---|---|"
} > "$SUMMARY"

# run_step NAME FUNCTION: runs it with output in NAME.log, records the result.
run_step() {
  local name="$1" fn="$2" t0 rc
  t0=$(date +%s)
  echo "== $name"
  # A subshell: a step's set -e / set -x stay inside it, and whatever it
  # changes on the machine is undone when it ends, however it ends.
  (
    UNDO=()
    BG=""
    trap cleanup EXIT
    trap on_interrupt INT TERM
    "$fn"
  ) > "$OUT/$name.log" 2>&1
  rc=$?
  LAST_RC=$rc
  if ((rc == 130)); then
    echo "   interrupted: the step's changes were undone"
    echo "| $name | interrupted | $name.log |" >> "$SUMMARY"
    exit 130
  fi
  local dt=$(( $(date +%s) - t0 ))
  if ((rc == 0)); then
    echo "   ok (${dt} s)"
    echo "| $name | ok (${dt} s) | $name.log |" >> "$SUMMARY"
  else
    echo "   FAILED (exit $rc, ${dt} s): see $OUT/$name.log"
    tail -5 "$OUT/$name.log" | sed 's/^/   | /'
    echo "| $name | **failed** (exit $rc) | $name.log |" >> "$SUMMARY"
  fi
  return 0
}

# ---------------------------------------------------------------------------
# Clean-up of everything the run changed (registered before changing it)
# ---------------------------------------------------------------------------
UNDO=()
undo_push() { UNDO+=("$1"); }
cleanup() {
  local k
  # Each in its own shell, so an undo command's variables cannot clash with ours.
  for ((k = ${#UNDO[@]} - 1; k >= 0; k--)); do
    bash -c "${UNDO[$k]}" >/dev/null 2>&1 || echo "clean-up step failed: ${UNDO[$k]}" >&2
  done
  UNDO=()
}
trap cleanup EXIT
trap 'echo "interrupted: cleaning up"; exit 130' INT TERM

# A command run as root through sudo does not get our Ctrl-C (sudo runs it on
# its own terminal and ignores signals from its own process group), and bash
# runs a trap only once the foreground command ends. So such commands run in
# the background under `wait` (which a signal interrupts), and the step's
# interrupt handler stops them itself.
as_root_bg() {
  sudo "$@" &
  BG=$!
  wait "$BG"
  local rc=$?
  BG=""
  return $rc
}
on_interrupt() {
  if [[ -n ${BG:-} ]]; then
    sudo -n pkill -TERM -P "$BG" 2>/dev/null
    kill "$BG" 2>/dev/null
    wait "$BG" 2>/dev/null
  fi
  exit 130
}

# ---------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------
step_host() {
  set -x
  uname -a
  cat /etc/os-release 2>/dev/null | head -4
  lscpu
  free -h
  nproc
  cat /proc/cmdline
  ulimit -l
  "$CORE/scripts/check-host.sh"
  "$CORE/scripts/check-host.sh" --json > "$OUT/host.json"
  "$CORE/scripts/tune-host.sh" --status
  ip -br link
  for dev in /sys/class/net/*; do
    d="${dev##*/}"
    [[ $d == lo ]] && continue
    echo "--- $d"
    ethtool -i "$d" 2>/dev/null
    ethtool -l "$d" 2>/dev/null
    ethtool -T "$d" 2>/dev/null
  done
  for t in cmake ninja g++ clang++ python3 docker; do
    command -v "$t" >/dev/null && echo "$t: $("$t" --version 2>&1 | head -1)" || echo "$t: missing"
  done
  pkg-config --modversion liburing 2>/dev/null || echo "liburing: missing (io_uring transport not built)"
  pkg-config --modversion librtlsdr 2>/dev/null || echo "librtlsdr: missing (live RTL-SDR source not built)"
}

step_build() {
  set -e
  cd "$CORE"
  for p in release bench naive-baseline; do
    cmake --preset "$p" -DADSB_WERROR=ON
    cmake --build --preset "$p" -j"$(nproc)"
  done
  grep -E "ADSB_HAVE_(IO_URING|AF_XDP)|LIBRTLSDR_FOUND" build/release/CMakeCache.txt || true
}

step_test() {
  set -e
  cd "$CORE"
  ctest --preset release -j"$(nproc)" --output-on-failure
  ./scripts/check-determinism.sh build/release build/naive-baseline
  cd "$ROOT"
  python3 protocol/gen.py --check
}

step_core() {
  set -e
  local work="$OUT/core-work" dur=120 n=3000 MB_QUICK=()
  ((QUICK)) && { dur=20; n=500; MB_QUICK=(--benchmark_min_time=0.05s); }
  mkdir -p "$work"
  echo "## microbenchmarks"
  "$BENCH/bench/adsb_bench" --benchmark_out="$OUT/microbench.json" --benchmark_out_format=json \
    ${MB_QUICK[@]+"${MB_QUICK[@]}"}
  echo "## synthetic traffic: $n aircraft, $dur s"
  "$REL/tools/adsb-synth" -o "$work/traffic.adsbrec" --aircraft "$n" --duration "$dur" --seed 2025
  for b in "$BENCH" "$NAIVE"; do
    echo "## replay, max speed ($(basename "$b"))"
    "$b/tools/adsb-replay" "$work/traffic.adsbrec"
    echo "## pipeline latency at 20x real time ($(basename "$b"))"
    "$b/tools/adsb-replay" "$work/traffic.adsbrec" --pipeline --speed 20 --busy-wait ${CPU_ARGS[@]+"${CPU_ARGS[@]}"}
  done
  echo "## overload suite"
  "$CORE/scripts/check-overload.sh" "$REL"
  echo "## shared-memory ring"
  for r in 10000 100000 1000000; do
    "$BENCH/tools/adsb-shm-bench" --rate "$r" --seconds $((QUICK ? 1 : 10)) ${CPU_ARGS[@]+"${CPU_ARGS[@]}"}
  done
  rm -rf "$work"
}

step_transports() {
  set -e
  "$BENCH/tools/adsb-tx-bench" --transport all --rate "$RATE" --size "$SIZE" --seconds "$SECONDS_PER" \
    --rounds "$ROUNDS" ${CPU_ARGS[@]+"${CPU_ARGS[@]}"}
}

# Refuses a port that is not a plain, idle device: an address, a route, a
# master (bond/bridge), or the address this session came in on.
check_port() {
  local d="$1"
  # LAB_ALLOW_VIRTUAL=1 lets the suite's own test use a veth pair.
  if [[ ! -e /sys/class/net/$d/device && ${LAB_ALLOW_VIRTUAL:-0} != 1 ]]; then
    echo "$d: not a physical NIC port (/sys/class/net/$d/device missing)"; return 1
  fi
  if [[ -n $(ip -o addr show dev "$d" 2>/dev/null | grep -E ' inet6? ' | grep -v ' fe80:') ]]; then
    echo "$d has an IP address: refusing to touch a port that is in use"; ip -br addr show dev "$d"; return 1
  fi
  if ip route show | grep -qw "dev $d"; then echo "$d carries a route: refusing"; return 1; fi
  if [[ -e /sys/class/net/$d/master ]]; then echo "$d belongs to a bond or bridge: refusing"; return 1; fi
  local local_ip="${SSH_CONNECTION:-}"; local_ip="$(awk '{print $3}' <<< "$local_ip")"
  if [[ -n $local_ip ]] && ip -o addr show dev "$d" | grep -qw "$local_ip"; then
    echo "$d holds this SSH session's address: refusing"; return 1
  fi
}

# Current combined (or rx) queue count, empty if the driver does not say.
queues() {
  sudo ethtool -l "$1" 2>/dev/null |
    awk '/Current hardware settings/ {c=1} c && /^Combined:/ {if ($2 ~ /^[0-9]+$/) print $2; exit}'
}

step_xdp() {
  check_port "$TX_PORT" || return 1
  check_port "$RX_PORT" || return 1
  if [[ -n $(ip -o addr show | grep -F " ${TX_IP%.*}.") ]] || [[ -n $(ip route show "${TX_IP%.*}.0/24") ]]; then
    echo "${TX_IP%.*}.0/24 is already in use on this machine: refusing"; return 1
  fi
  set -x
  ip netns list | grep -qw "$NS" && { echo "namespace $NS exists already (an earlier run did not finish?): sudo ip netns del $NS"; return 1; }
  local tx_up rx_up tx_mtu rx_mtu tx_q rx_q
  tx_up=$(cat "/sys/class/net/$TX_PORT/flags"); rx_up=$(cat "/sys/class/net/$RX_PORT/flags")
  tx_mtu=$(cat "/sys/class/net/$TX_PORT/mtu"); rx_mtu=$(cat "/sys/class/net/$RX_PORT/mtu")
  tx_q=$(queues "$TX_PORT"); rx_q=$(queues "$RX_PORT")
  echo "before: $TX_PORT flags $tx_up mtu $tx_mtu queues ${tx_q:-?}; $RX_PORT flags $rx_up mtu $rx_mtu queues ${rx_q:-?}"

  # Back to how it was, newest change first (cleanup runs these in reverse).
  undo_push "sudo ip link set $TX_PORT mtu $tx_mtu; sudo ip link set $RX_PORT mtu $rx_mtu"
  [[ -n $tx_q ]] && undo_push "sudo ethtool -L $TX_PORT combined $tx_q"
  [[ -n $rx_q ]] && undo_push "sudo ethtool -L $RX_PORT combined $rx_q"
  (( (tx_up & 1) == 0 )) && undo_push "sudo ip link set $TX_PORT down"
  (( (rx_up & 1) == 0 )) && undo_push "sudo ip link set $RX_PORT down"
  undo_push "sudo ip addr del $TX_IP/24 dev $TX_PORT"
  # (usually gone with the address already)
  undo_push "sudo ip neigh del $RX_IP dev $TX_PORT 2>/dev/null || true"
  # Move the port back first (deleting the namespace would return a physical
  # port only asynchronously, and would destroy a virtual one).
  undo_push "sudo ip -n $NS link set $RX_PORT netns 1; sudo ip netns del $NS"

  sudo ip netns add "$NS" || return 1
  sudo ip link set "$RX_PORT" netns "$NS" || return 1
  local X=(sudo ip netns exec "$NS")
  ((tx_mtu > 1500)) && sudo ip link set "$TX_PORT" mtu 1500
  ((rx_mtu > 1500)) && "${X[@]}" ip link set "$RX_PORT" mtu 1500
  # One queue: the AF_XDP sockets bind queue 0, and frames hashed to another
  # queue would reach only the kernel.
  [[ -n $tx_q && $tx_q != 1 ]] && { sudo ethtool -L "$TX_PORT" combined 1 || echo "warning: $TX_PORT kept $tx_q queues"; }
  [[ -n $rx_q && $rx_q != 1 ]] && { "${X[@]}" ethtool -L "$RX_PORT" combined 1 || echo "warning: $RX_PORT kept $rx_q queues"; }
  sudo ip addr add "$TX_IP/24" dev "$TX_PORT"
  "${X[@]}" ip addr add "$RX_IP/24" dev "$RX_PORT"
  sudo ip link set "$TX_PORT" up
  "${X[@]}" ip link set "$RX_PORT" up
  "${X[@]}" ip link set lo up

  for _ in $(seq 100); do
    [[ $(cat "/sys/class/net/$TX_PORT/carrier" 2>/dev/null) == 1 ]] && break
    sleep 0.1
  done
  [[ $(cat "/sys/class/net/$TX_PORT/carrier" 2>/dev/null) == 1 ]] || { echo "$TX_PORT has no link: are the two ports cabled to each other?"; return 1; }
  local tx_mac rx_mac
  tx_mac=$(cat "/sys/class/net/$TX_PORT/address")
  rx_mac=$("${X[@]}" cat "/sys/class/net/$RX_PORT/address")
  sudo ip neigh replace "$RX_IP" lladdr "$rx_mac" dev "$TX_PORT" nud permanent
  "${X[@]}" ip neigh replace "$TX_IP" lladdr "$tx_mac" dev "$RX_PORT" nud permanent
  ethtool -i "$TX_PORT"
  "${X[@]}" ethtool -i "$RX_PORT"
  # A short kernel-UDP check that the cable carries traffic before measuring.
  ping -c 3 -W 1 "$RX_IP" || { echo "no reply from $RX_IP over the cable"; return 1; }
  set +x

  local base=(as_root_bg "$BENCH/tools/adsb-tx-bench" --interface "$TX_IP" --dest "$RX_IP" --rx-netns "$NS"
              --rate "$RATE" --size "$SIZE" --seconds "$SECONDS_PER" --rounds "$ROUNDS" ${CPU_ARGS[@]+"${CPU_ARGS[@]}"})
  local rc=0 best=copy
  echo "## AF_XDP copy mode, both ends (kernel transports in the same rounds as the control)"
  "${base[@]}" --transport all --rx all --xdp-mode copy || rc=1
  echo "## AF_XDP zero-copy mode, both ends"
  if "${base[@]}" --transport all --rx all --xdp-mode zerocopy; then
    best=zerocopy
  else
    echo "zero-copy: not available on this driver/port (the copy-mode numbers above stand)"
  fi
  echo "## ten times the rate, $best mode"
  "${base[@]}" --transport kernel,xdp --rx all --xdp-mode "$best" --rate $((RATE * 10)) || rc=1
  echo "## NIC counters after the run"
  ethtool -S "$TX_PORT" 2>/dev/null | grep -iE "xdp|xsk|drop|err" | grep -v ": 0$" | head -40
  "${X[@]}" ethtool -S "$RX_PORT" 2>/dev/null | grep -iE "xdp|xsk|drop|err" | grep -v ": 0$" | head -40
  return $rc
}

step_legacy() {
  set -e
  command -v docker >/dev/null || { echo "docker is missing: the legacy step needs it"; return 1; }
  local env=(RESULTS_DIR="$OUT/legacy")
  ((QUICK)) && env+=(SCENARIOS="50 200" DURATION=30)
  env "${env[@]}" "$ROOT/bench/legacy/run.sh"
}

# Transponder frequency study (plan 22.5): a recording at 1090 MHz, every
# message's carrier frequency (adsb-carrier), then per aircraft
# (carrier-study.py). The receiver's own error is estimated from the
# aircraft (an RTL-SDR's crystal is not a reference) and its drift removed.
step_carrier() {
  set -e
  local work="$OUT/carrier" secs="$SDR_SECONDS" input fmt_args=()
  ((QUICK)) && secs=20
  mkdir -p "$work"
  if [[ -n $IQ_IN ]]; then
    input="$IQ_IN"
    [[ $input == *.sigmf-meta || $input == *.sigmf-data ]] || fmt_args=(--format cu8 --rate 2.4e6)
  else
    command -v rtl_sdr >/dev/null || { echo "rtl_sdr is missing (apt install rtl-sdr), or pass --iq FILE"; return 1; }
    local need=$((secs * 4800000 + 1000000000)) avail
    avail=$(( $(df -Pk "$work" | awk 'NR == 2 {print $4}') * 1024 ))
    ((avail > need)) || { echo "not enough disk for $secs s of IQ: need $need bytes, $avail free"; return 1; }
    echo "## recording $secs s at 2.4 MS/s, gain $SDR_GAIN (device $SDR_DEVICE)"
    local start
    start=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    rtl_sdr -d "$SDR_DEVICE" -f 1090000000 -s 2400000 -g "$SDR_GAIN" -n $((secs * 2400000)) \
      "$work/capture.sigmf-data" 2> "$work/rtl_sdr.log" || { cat "$work/rtl_sdr.log"; return 1; }
    cat "$work/rtl_sdr.log"
    if grep -qi "lost" "$work/rtl_sdr.log"; then
      echo "WARNING: rtl_sdr lost samples (USB): message times and Doppler around the gaps are off"
    fi
    IFS=, read -r lat lon alt <<< "$RECEIVER"
    cat > "$work/capture.sigmf-meta" <<EOF2
{
  "global": {
    "core:datatype": "cu8",
    "core:sample_rate": 2400000,
    "core:version": "1.0.0",
    "core:recorder": "rtl_sdr",
    "core:hw": "RTL-SDR device $SDR_DEVICE, gain $SDR_GAIN dB",
    "core:description": "1090 MHz recording for the transponder frequency study (bench/lab/run.sh --steps carrier)",
    "adsb:receiver": {"lat": $lat, "lon": $lon, "ground_alt_m": ${alt:-0}, "antenna_height_m": 0}
  },
  "captures": [{"core:sample_start": 0, "core:frequency": 1090000000, "core:datetime": "$start"}],
  "annotations": []
}
EOF2
    input="$work/capture.sigmf-meta"
  fi
  echo "## adsb-carrier"
  "$REL/tools/adsb-carrier" "$input" ${fmt_args[@]+"${fmt_args[@]}"} --receiver "$RECEIVER" --out "$work/messages.csv"
  echo "## ADC clipping (too much gain bends the phase of strong aircraft)"
  python3 - "$work/messages.csv" <<'EOF2'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
clipped = sum(1 for r in rows if int(r["clipped"]) > 0)
share = clipped / len(rows) if rows else 0.0
print(f"frames with clipped samples: {clipped} of {len(rows)} ({100 * share:.1f}%)")
if share > 0.05:
    print("WARNING: more than 5% of the frames clip: lower --sdr-gain")
EOF2
  echo "## carrier-study.py (receiver ppm estimated from the aircraft, drift removed in 10 s bins)"
  local study=(python3 "$CORE/scripts/carrier-study.py" "$work/messages.csv" --rx-drift 10
               --types "$ROOT/ads/aircraft-types-by-hex.json")
  "${study[@]}" --aircraft-out "$work/aircraft.csv" | tee "$work/study.txt"
  "${study[@]}" --json > "$work/study.json"
  ((KEEP_IQ)) || rm -f "$work/capture.sigmf-data"
}

# Ask for sudo once, up front, not in the middle of a logged step.
if [[ -n $TUNE ]] || want xdp; then
  echo "sudo is needed for: ${TUNE:+tuning }$(want xdp && echo 'the xdp step')"
  sudo -v || { echo "no sudo: drop --tune and the xdp step" >&2; exit 1; }
  # Keep it fresh during long runs; stops with this script.
  ( while kill -0 $$ 2>/dev/null; do sudo -n true; sleep 50; done ) &
  KEEPALIVE=$!
  undo_push "kill $KEEPALIVE"
fi

if [[ -n $TUNE ]]; then
  echo "== tune (sudo core/scripts/tune-host.sh --apply --cpus $TUNE)"
  # shellcheck disable=SC2024  # the log belongs to the user, not root
  if sudo "$CORE/scripts/tune-host.sh" --apply --cpus "$TUNE" > "$OUT/tune.log" 2>&1; then
    undo_push "sudo '$CORE/scripts/tune-host.sh' --revert >> '$OUT/tune.log' 2>&1"
    echo "| tune | applied, reverted at the end | tune.log |" >> "$SUMMARY"
  else
    echo "   tuning failed (see tune.log); measuring untuned"
    echo "| tune | **failed**, ran untuned | tune.log |" >> "$SUMMARY"
  fi
fi

BUILT=1
for s in $ALL_STEPS; do
  want "$s" || continue
  # Everything but host and legacy runs what the build step built.
  if ((!BUILT)) && [[ $s != host && $s != legacy ]]; then
    echo "== $s"; echo "   skipped: the build failed"
    echo "| $s | skipped (build failed) | |" >> "$SUMMARY"
    continue
  fi
  run_step "$s" "step_$s"
  [[ $s == build && $LAST_RC != 0 ]] && BUILT=0
done
cleanup

# The key lines of each log, so SUMMARY.md alone gives the picture.
{
  echo
  echo "## Key lines"
  for f in core transports xdp carrier; do
    [[ -f $OUT/$f.log ]] || continue
    echo
    echo "### $f"
    echo '```'
    grep -E "^##|^build |records/s|p50|p99|frames/s|msg/s|PASS|FAIL|bound|mode|lost|receiver|aircraft with|offset at|drift|clipped|WARNING" "$OUT/$f.log" |
      grep -v "^+" | head -120
    echo '```'
  done
} >> "$SUMMARY"

# The raw IQ (kept with --keep-iq) stays out of the archive: gigabytes.
tar -C "$(dirname "$OUT")" --exclude='*.sigmf-data' -czf "$OUT.tar.gz" "$(basename "$OUT")"
echo
echo "done: $SUMMARY"
echo "copy back: $OUT.tar.gz"
