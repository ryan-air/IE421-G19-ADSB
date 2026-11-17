#!/usr/bin/env bash
# check-host.sh — report whether this machine can run the project's
# low-latency experiments (README_UPDATES.md §18, §21 "Open Decisions").
#
# Read-only: it only inspects /proc, /sys, and tool output. No root needed,
# though `ethtool -T` and some /sys files give more detail with sudo.
#
# Usage:  scripts/check-host.sh            human-readable report
#         scripts/check-host.sh --json     machine-readable (for bench/results)
#
# Exit status is 0 even when capabilities are missing; the report is the output.

set -u

JSON=0
[[ "${1:-}" == "--json" ]] && JSON=1

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
KEYS=()
VALS=()
STATUSES=()

# record <key> <status: ok|warn|no|info> <value...>
record() {
  local key="$1" status="$2"
  shift 2
  KEYS+=("$key")
  STATUSES+=("$status")
  VALS+=("$*")
}

have() { command -v "$1" >/dev/null 2>&1; }

json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  s="${s//$'\n'/ }"
  s="${s//$'\t'/ }"
  printf '%s' "$s"
}

read_file() { [[ -r "$1" ]] && cat "$1" 2>/dev/null; }

OS="$(uname -s)"
ARCH="$(uname -m)"

# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------
record "os" info "$OS $(uname -r)"
record "arch" info "$ARCH"

if [[ "$OS" != "Linux" ]]; then
  record "linux" no "Not Linux: latency benchmarks, isolcpus, AF_XDP, PTP, resctrl, and perf all require Linux. Use this machine for development and unit tests only."
  if [[ "$OS" == "Darwin" ]]; then
    record "cpu_model" info "$(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
    record "cpu_count" info "$(sysctl -n hw.physicalcpu 2>/dev/null) physical / $(sysctl -n hw.logicalcpu 2>/dev/null) logical"
    record "cache_line" info "$(sysctl -n hw.cachelinesize 2>/dev/null) bytes"
  fi
else
  record "linux" ok "yes"

  # Kernel version gates: AF_XDP need_wakeup (5.4), io_uring (5.1; SQPOLL
  # without root 5.11).
  KVER="$(uname -r | cut -d. -f1-2)"
  KMAJ="${KVER%%.*}"
  KMIN="${KVER#*.}"
  KMIN="${KMIN%%[^0-9]*}"
  if (( KMAJ > 5 || (KMAJ == 5 && KMIN >= 11) )); then
    record "kernel" ok "$KVER (io_uring SQPOLL and AF_XDP need_wakeup supported)"
  elif (( KMAJ == 5 && KMIN >= 4 )); then
    record "kernel" warn "$KVER (AF_XDP ok; io_uring SQPOLL needs root before 5.11)"
  else
    record "kernel" no "$KVER (too old for AF_XDP/io_uring experiments; need >= 5.4)"
  fi

  # Raspberry Pi model, if any.
  if [[ -r /proc/device-tree/model ]]; then
    record "board" info "$(tr -d '\0' </proc/device-tree/model)"
  fi

  # CPU
  is_num() { [[ "$1" =~ ^[0-9]+$ ]]; }
  if have lscpu; then
    MODEL="$(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); print $2; exit}')"
    if [[ -z "$MODEL" || "$MODEL" == "-" ]]; then
      # Some ARM kernels report "-": fall back to vendor + part, or the board.
      MODEL="$(lscpu | awk -F: '/Vendor ID/ {gsub(/^ +/, "", $2); v=$2} /^Model:/ {gsub(/^ +/, "", $2); m=$2} END {print v " model " m}')"
    fi
    record "cpu_model" info "$MODEL"
    SOCKETS="$(lscpu | awk -F: '/Socket\(s\)/ {gsub(/ /, "", $2); print $2; exit}')"
    CORES_PER="$(lscpu | awk -F: '/Core\(s\) per (socket|cluster)/ {gsub(/ /, "", $2); print $2; exit}')"
    NUMA="$(lscpu | awk -F: '/NUMA node\(s\)/ {gsub(/ /, "", $2); print $2; exit}')"
    is_num "$SOCKETS" || SOCKETS=1
    if is_num "$CORES_PER"; then
      PHYS=$(( SOCKETS * CORES_PER ))
    else
      # Count distinct (package, core) pairs from sysfs; fall back to nproc.
      PHYS="$(for t in /sys/devices/system/cpu/cpu[0-9]*/topology; do
                echo "$(cat "$t/physical_package_id" 2>/dev/null):$(cat "$t/core_id" 2>/dev/null)"
              done | sort -u | wc -l | tr -d ' ')"
      is_num "$PHYS" && (( PHYS > 0 )) || PHYS="$(nproc 2>/dev/null || echo 0)"
    fi
    if (( PHYS >= 8 )); then
      record "physical_cores" ok "$PHYS (sockets=$SOCKETS, numa_nodes=${NUMA:-?})"
    elif (( PHYS >= 4 )); then
      record "physical_cores" warn "$PHYS (>= 8 recommended for one isolated core per stage + A/B lines)"
    else
      record "physical_cores" no "$PHYS"
    fi
  fi
  record "logical_cpus" info "$(nproc 2>/dev/null)"

  SMT="$(read_file /sys/devices/system/cpu/smt/active)"
  if [[ "$SMT" == "1" ]]; then
    record "smt" warn "active (disable for benchmark runs: echo off > /sys/devices/system/cpu/smt/control)"
  elif [[ "$SMT" == "0" ]]; then
    record "smt" ok "inactive"
  else
    record "smt" info "unknown / not supported"
  fi

  FLAGS="$(grep -m1 -E '^(flags|Features)' /proc/cpuinfo 2>/dev/null | cut -d: -f2)"
  has_flag() { [[ " $FLAGS " == *" $1 "* ]]; }

  if [[ "$ARCH" == "x86_64" ]]; then
    if has_flag constant_tsc && has_flag nonstop_tsc; then
      record "tsc" ok "invariant (constant_tsc + nonstop_tsc): ticks() is usable"
    else
      record "tsc" no "not invariant: use mono_ns() instead of ticks() for timing"
    fi
    # Intel RDT / AMD PQoS for cache and memory-bandwidth partitioning (§13).
    RDT=()
    has_flag cat_l3 && RDT+=("cat_l3")
    has_flag cdp_l3 && RDT+=("cdp_l3")
    has_flag mba && RDT+=("mba")
    has_flag cqm_llc && RDT+=("cqm_llc")
    if has_flag cat_l3; then
      record "rdt" ok "${RDT[*]}"
    else
      record "rdt" no "no L3 cache allocation (cat_l3): noisy-neighbor CAT/MBA rows not possible"
    fi
    has_flag avx2 && record "avx2" ok "yes (SIMD demodulator path)" || record "avx2" no "no"
    has_flag avx512f && record "avx512" info "yes"
  elif [[ "$ARCH" == "aarch64" ]]; then
    record "tsc" ok "AArch64 generic timer (CNTVCT_EL0), fixed frequency"
    has_flag asimd && record "neon" ok "yes (SIMD demodulator path)" || record "neon" no "no"
    record "rdt" no "not available on ARM (no CAT/MBA rows)"
  fi

  if [[ -d /sys/fs/resctrl ]] && grep -q resctrl /proc/mounts 2>/dev/null; then
    record "resctrl" ok "mounted at /sys/fs/resctrl"
  elif [[ -d /sys/fs/resctrl ]]; then
    record "resctrl" warn "supported but not mounted (mount -t resctrl resctrl /sys/fs/resctrl)"
  else
    record "resctrl" info "not present"
  fi

  # Kernel command line: core isolation (§4 design rules).
  CMDLINE="$(read_file /proc/cmdline)"
  for opt in isolcpus nohz_full rcu_nocbs; do
    val="$(tr ' ' '\n' <<<"$CMDLINE" | grep -m1 "^${opt}=" | cut -d= -f2-)"
    if [[ -n "$val" ]]; then
      record "$opt" ok "$val"
    else
      record "$opt" warn "not set"
    fi
  done

  GOV="$(read_file /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  if [[ "$GOV" == "performance" ]]; then
    record "governor" ok "performance"
  elif [[ -n "$GOV" ]]; then
    record "governor" warn "$GOV (set to performance for benchmarks)"
  else
    record "governor" info "no cpufreq (VM or fixed frequency)"
  fi

  THP="$(read_file /sys/kernel/mm/transparent_hugepage/enabled)"
  [[ -n "$THP" ]] && record "thp" info "$THP"
  HP="$(awk '/HugePages_Total/ {print $2}' /proc/meminfo 2>/dev/null)"
  if [[ "${HP:-0}" -gt 0 ]]; then
    record "hugepages_2m" ok "$HP reserved"
  else
    record "hugepages_2m" warn "0 reserved (echo 512 > /proc/sys/vm/nr_hugepages)"
  fi

  IOU="$(read_file /proc/sys/kernel/io_uring_disabled)"
  case "$IOU" in
    1|2) record "io_uring" no "disabled by sysctl kernel.io_uring_disabled=$IOU" ;;
    *) record "io_uring" ok "enabled" ;;
  esac

  # Virtualization hurts latency measurements.
  if have systemd-detect-virt; then
    VIRT="$(systemd-detect-virt 2>/dev/null || true)"
    if [[ -n "$VIRT" && "$VIRT" != "none" ]]; then
      record "virtualization" warn "$VIRT (bare metal preferred for latency numbers)"
    else
      record "virtualization" ok "bare metal"
    fi
  fi

  # -------------------------------------------------------------------------
  # NICs: hardware timestamping, PTP clock, AF_XDP zero-copy driver (§9.3-9.4)
  # -------------------------------------------------------------------------
  # Drivers with upstream AF_XDP zero-copy support, and drivers where it
  # depends on the kernel version (added relatively recently). Verify with
  # the xdpsock sample or our AF_XDP transport's startup check.
  ZC_DRIVERS=" i40e ice igc ixgbe mlx5_core stmmac "
  ZC_MAYBE=" igb ena virtio_net "
  for dev in /sys/class/net/*; do
    ifc="$(basename "$dev")"
    [[ "$ifc" == "lo" ]] && continue
    [[ -e "$dev/device" ]] || continue  # skip virtual interfaces (bridges, veth)
    driver="$(basename "$(readlink -f "$dev/device/driver" 2>/dev/null)" 2>/dev/null)"
    speed="$(read_file "$dev/speed")"
    state="$(read_file "$dev/operstate")"
    desc="driver=${driver:-?} speed=${speed:-?}Mb/s state=${state:-?}"

    zc="no"
    [[ " $ZC_DRIVERS " == *" $driver "* ]] && zc="yes"
    [[ " $ZC_MAYBE " == *" $driver "* ]] && zc="kernel-dependent"

    hwts="unknown (install ethtool)"
    phc=""
    if have ethtool; then
      TS="$(ethtool -T "$ifc" 2>/dev/null)"
      if grep -q "hardware-transmit" <<<"$TS" && grep -q "hardware-receive" <<<"$TS"; then
        hwts="yes"
      elif [[ -n "$TS" ]]; then
        hwts="no"
      fi
      phc="$(awk -F: '/PTP Hardware Clock/ {gsub(/ /, "", $2); print $2}' <<<"$TS")"
    fi

    status="warn"
    [[ "$hwts" == "yes" && "$zc" == "yes" ]] && status="ok"
    [[ "$hwts" == "no" && "$zc" == "no" ]] && status="no"
    record "nic_${ifc}" "$status" "$desc hw_timestamps=$hwts ptp_clock=${phc:-none} af_xdp_zero_copy_driver=$zc"
  done

  if have ptp4l; then
    record "linuxptp" ok "ptp4l installed"
  else
    record "linuxptp" warn "not installed (apt install linuxptp)"
  fi
fi

# ---------------------------------------------------------------------------
# Tools (any OS)
# ---------------------------------------------------------------------------
for tool in cmake ninja clang++ g++ perf bpftrace ethtool; do
  if have "$tool"; then
    record "tool_$tool" ok "$(command -v "$tool")"
  else
    record "tool_$tool" info "not found"
  fi
done

# RTL-SDR dongles (USB IDs 0bda:2838 / 0bda:2832).
if have lsusb; then
  N_SDR="$(lsusb 2>/dev/null | grep -ciE '0bda:283[28]')"
  if (( N_SDR >= 2 )); then
    record "rtl_sdr" ok "$N_SDR dongles (A/B lines possible)"
  elif (( N_SDR == 1 )); then
    record "rtl_sdr" warn "1 dongle (second needed for A/B arbitration)"
  else
    record "rtl_sdr" info "none detected"
  fi
fi

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
if (( JSON )); then
  printf '{\n  "host": "%s",\n  "checks": {\n' "$(json_escape "$(hostname)")"
  for i in "${!KEYS[@]}"; do
    sep=","
    (( i == ${#KEYS[@]} - 1 )) && sep=""
    printf '    "%s": {"status": "%s", "value": "%s"}%s\n' \
      "$(json_escape "${KEYS[$i]}")" "${STATUSES[$i]}" "$(json_escape "${VALS[$i]}")" "$sep"
  done
  printf '  }\n}\n'
else
  if [[ -t 1 ]]; then
    C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_NO=$'\033[31m'; C_INFO=$'\033[36m'; C_END=$'\033[0m'
  else
    C_OK=""; C_WARN=""; C_NO=""; C_INFO=""; C_END=""
  fi
  printf 'Host capability report: %s\n\n' "$(hostname)"
  n_ok=0; n_warn=0; n_no=0
  for i in "${!KEYS[@]}"; do
    case "${STATUSES[$i]}" in
      ok)   tag="${C_OK}[ ok ]${C_END}"; n_ok=$((n_ok + 1)) ;;
      warn) tag="${C_WARN}[warn]${C_END}"; n_warn=$((n_warn + 1)) ;;
      no)   tag="${C_NO}[ no ]${C_END}"; n_no=$((n_no + 1)) ;;
      *)    tag="${C_INFO}[info]${C_END}" ;;
    esac
    printf '%s %-18s %s\n' "$tag" "${KEYS[$i]}" "${VALS[$i]}"
  done
  printf '\n%d ok, %d warnings, %d missing\n' "$n_ok" "$n_warn" "$n_no"
fi
exit 0
