#!/usr/bin/env bash
# tune-host.sh — put a Linux benchmark host into a reproducible low-latency
# state for the pipeline's latency runs, and undo it (README_UPDATES.md §4
# "Design rules for the hot path", §11.3 methodology, §18 benchmark host).
#
# Companion to check-host.sh (which only reports). This script changes
# runtime kernel settings, records the original values first, and can put
# them back. It never edits bootloader configuration: boot-time options
# (isolcpus, nohz_full, rcu_nocbs, irqaffinity) are printed, not applied.
#
# Default mode is --dry-run: print the exact commands, change nothing.
# Run with --help for the full option list.

set -u

PROG="${0##*/}"

MODE="dry-run"
MODE_SET=0
CPUS_ARG=""
HUGEPAGES=512
SMT_OFF=0
NO_TURBO=0
ALLOW_CPU0=0
FORCE=0
STATE_FILE="/var/tmp/adsb-tune-host.state"
STAT_INTERVAL=10

usage() {
  cat <<EOF
Usage: $PROG [MODE] [OPTIONS]

Put a Linux machine into a reproducible low-latency benchmarking state,
and undo it. Linux only; on other systems it explains that and does nothing.

Modes (pick one):
  --dry-run        (default) Print every change --apply would make, as the
                   exact shell commands, and change nothing. No root needed.
  --apply          Make the changes. Requires root. Saves every original
                   value to the state file BEFORE changing it.
  --revert         Restore the values recorded by --apply from the state
                   file, then delete it. Requires root.
  --status         Show the current value of everything this script manages.

Options:
  --cpus LIST      Cores reserved for the pipeline, e.g. 2-5 or 2,3,6.
                   Required for --apply. Validated against online CPUs.
  --hugepages N    2 MB huge pages to reserve (vm.nr_hugepages). Default 512.
                   Never lowers an existing larger reservation.
  --smt-off        Opt-in: disable SMT (/sys/devices/system/cpu/smt/control).
                   Each reserved CPU must be the first thread of its core.
  --no-turbo       Opt-in: disable turbo/boost (intel_pstate/no_turbo=1, or
                   cpufreq/boost=0), whichever exists.
  --allow-cpu0     Permit CPU 0 in --cpus (refused by default: it handles
                   timers, many IRQs, and housekeeping work).
  --state FILE     State file. Default: /var/tmp/adsb-tune-host.state
  --force          With --apply, proceed even if a state file exists (the old
                   file is moved to FILE.prev). Use only if you are sure the
                   existing file no longer describes the true originals.
  -h, --help       Show this help.

Runtime changes managed (each skipped with a note if its file is absent):
  - CPU frequency governor -> performance on every online CPU
      /sys/devices/system/cpu/cpuN/cpufreq/scaling_governor
  - IRQ affinity: every IRQ in /proc/irq/*/smp_affinity_list is moved off
      the reserved cores (its current set minus the reserved cores, or all
      non-reserved cores if that is empty); IRQs that refuse the write
      (kernel-managed, per-CPU) are skipped and listed.
      /proc/irq/default_smp_affinity is set likewise.
  - irqbalance (it would undo the IRQ affinity): on --apply, if systemd
      reports it active it is stopped and --revert restarts it. A
      non-systemd irqbalance is only warned about.
  - vm.nr_hugepages -> N; transparent hugepages -> madvise
  - kernel.timer_migration=0, vm.stat_interval=$STAT_INTERVAL,
      kernel.perf_event_paranoid=1 (only if currently > 1, so perf works)
  - kernel.sched_rt_runtime_us is reported, never changed
  - optional: SMT off (--smt-off), turbo off (--no-turbo)
Nothing else is touched.

Boot-time settings (printed, never applied): the kernel command-line
additions for the reserved cores,
  isolcpus=managed_irq,domain,LIST nohz_full=LIST rcu_nocbs=LIST irqaffinity=REST
and whether /proc/cmdline already contains them.

Exit status:
  0  success
  1  usage error (bad option, bad CPU list, state file conflict, non-Linux
     for --apply/--revert)
  2  not root for --apply / --revert
  3  partial failure: some writes failed (summary printed)
EOF
}

die_usage() {
  printf '%s: %s\n' "$PROG" "$1" >&2
  printf 'Run %s --help for usage.\n' "$PROG" >&2
  exit 1
}

set_mode() {
  if (( MODE_SET )) && [[ "$MODE" != "$1" ]]; then
    die_usage "choose only one of --dry-run, --apply, --revert, --status"
  fi
  MODE="$1"
  MODE_SET=1
}

need_arg() { [[ $# -ge 2 && -n "$2" ]] || die_usage "$1 needs a value"; }

while (( $# )); do
  case "$1" in
    --dry-run) set_mode dry-run ;;
    --apply) set_mode apply ;;
    --revert) set_mode revert ;;
    --status) set_mode status ;;
    --cpus) need_arg "$@"; CPUS_ARG="$2"; shift ;;
    --cpus=*) CPUS_ARG="${1#*=}" ;;
    --hugepages) need_arg "$@"; HUGEPAGES="$2"; shift ;;
    --hugepages=*) HUGEPAGES="${1#*=}" ;;
    --state) need_arg "$@"; STATE_FILE="$2"; shift ;;
    --state=*) STATE_FILE="${1#*=}" ;;
    --smt-off) SMT_OFF=1 ;;
    --no-turbo) NO_TURBO=1 ;;
    --allow-cpu0) ALLOW_CPU0=1 ;;
    --force) FORCE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die_usage "unknown option: $1" ;;
  esac
  shift
done

[[ "$HUGEPAGES" =~ ^[0-9]+$ ]] || die_usage "--hugepages must be a non-negative integer (got '$HUGEPAGES')"
HUGEPAGES=$((10#$HUGEPAGES))
[[ -n "$STATE_FILE" ]] || die_usage "--state needs a file name"
if [[ -n "$CPUS_ARG" ]] && ! [[ "$CPUS_ARG" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]]; then
  die_usage "--cpus: '$CPUS_ARG' is not a CPU list (examples: 2-5, 2,3,6, 2-3,6)"
fi
if [[ "$MODE" == "apply" && -z "$CPUS_ARG" ]]; then
  die_usage "--apply needs --cpus LIST (the cores reserved for the pipeline)"
fi

# ---------------------------------------------------------------------------
# Output helpers (same tag style as check-host.sh)
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
  C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_NO=$'\033[31m'; C_INFO=$'\033[36m'; C_DO=$'\033[35m'; C_END=$'\033[0m'
else
  C_OK=""; C_WARN=""; C_NO=""; C_INFO=""; C_DO=""; C_END=""
fi
ok()      { printf '%s %s\n' "${C_OK}[ ok ]${C_END}" "$*"; }
warn()    { printf '%s %s\n' "${C_WARN}[warn]${C_END}" "$*"; }
info()    { printf '%s %s\n' "${C_INFO}[info]${C_END}" "$*"; }
skip()    { printf '%s %s\n' "${C_INFO}[skip]${C_END}" "$*"; }
would()   { printf '%s %s\n' "${C_DO}[ do ]${C_END}" "$*"; }
done_()   { printf '%s %s\n' "${C_OK}[done]${C_END}" "$*"; }
fail()    { printf '%s %s\n' "${C_NO}[FAIL]${C_END}" "$*"; }
section() { printf '\n== %s ==\n' "$1"; }

N_CHANGED=0
N_FAILED=0
FAILED_LIST=""
N_REFUSED=0
REFUSED_LIST=""

record_failure() {
  N_FAILED=$((N_FAILED + 1))
  FAILED_LIST="${FAILED_LIST}  - $1"$'\n'
}

have() { command -v "$1" >/dev/null 2>&1; }

# Read a sysfs/procfs value. "always [madvise] never" style files yield the
# bracketed (selected) word.
read_val() {
  local v
  v="$(cat "$1" 2>/dev/null)" || return 1
  v="${v%%$'\n'*}"
  if [[ "$v" == *"["*"]"* ]]; then
    v="${v#*\[}"
    v="${v%%\]*}"
  fi
  printf '%s' "$v"
}

# write_file PATH VALUE: prints the error text and returns non-zero on failure.
write_file() {
  local err
  err="$( { printf '%s\n' "$2" > "$1"; } 2>&1 )" && return 0
  err="${err##*: }"
  printf '%s' "${err:-write failed}"
  return 1
}

# ---------------------------------------------------------------------------
# CPU list helpers
# ---------------------------------------------------------------------------
# expand_list "2-4,7" -> "2 3 4 7" (sorted, unique)
expand_list() {
  local spec="${1//,/ }" part a b out=""
  for part in $spec; do
    if [[ "$part" == *-* ]]; then a="${part%-*}"; b="${part#*-}"; else a="$part"; b="$part"; fi
    [[ "$a" =~ ^[0-9]+$ && "$b" =~ ^[0-9]+$ ]] || continue
    a=$((10#$a)); b=$((10#$b))
    while (( a <= b )); do out="$out $a"; a=$((a + 1)); done
  done
  [[ -n "$out" ]] || return 0
  # shellcheck disable=SC2086  # intentional word splitting of a number list
  printf '%s\n' $out | sort -n -u | tr '\n' ' ' | sed 's/ $//'
}

# compress_list "0 1 2 5" -> "0-2,5"
compress_list() {
  local out="" start="" prev="" c
  for c in $1; do
    if [[ -z "$start" ]]; then start=$c; prev=$c; continue; fi
    if (( c == prev + 1 )); then prev=$c; continue; fi
    if (( start == prev )); then out="$out,$start"; else out="$out,$start-$prev"; fi
    start=$c; prev=$c
  done
  if [[ -n "$start" ]]; then
    if (( start == prev )); then out="$out,$start"; else out="$out,$start-$prev"; fi
  fi
  printf '%s' "${out#,}"
}

in_list() { [[ " $2 " == *" $1 "* ]]; }

# list_minus "A" "B" -> elements of A not in B
list_minus() {
  local c out=""
  for c in $1; do in_list "$c" "$2" || out="$out $c"; done
  printf '%s' "${out# }"
}

list_intersects() {
  local c
  for c in $1; do in_list "$c" "$2" && return 0; done
  return 1
}

# Kernel bitmap ("ffffffff,0000000f") -> CPU list.
mask_to_list() {
  local hex="${1//,/}" n i d bit out=""
  hex="${hex//[^0-9a-fA-F]/}"
  n=${#hex}
  for (( i = 0; i < n; i++ )); do
    d=$(( 16#${hex:n-1-i:1} ))
    for bit in 0 1 2 3; do
      (( d & (1 << bit) )) && out="$out $(( i * 4 + bit ))"
    done
  done
  printf '%s' "${out# }"
}

# CPU list -> kernel bitmap with 32-bit comma-separated words.
list_to_mask() {
  local max=0 c w word out=""
  for c in $1; do (( c > max )) && max=$c; done
  for (( w = max / 32; w >= 0; w-- )); do
    word=0
    for c in $1; do
      (( c / 32 == w )) && word=$(( word | (1 << (c % 32)) ))
    done
    if [[ -z "$out" ]]; then out="$(printf '%x' "$word")"; else out="$out,$(printf '%08x' "$word")"; fi
  done
  printf '%s' "$out"
}

# IRQ numbers with an smp_affinity_list, in numeric order.
irq_numbers() {
  local f
  for f in /proc/irq/[0-9]*/smp_affinity_list; do
    [[ -e "$f" ]] || continue
    f="${f#/proc/irq/}"
    printf '%s\n' "${f%%/*}"
  done | sort -n
}

# ---------------------------------------------------------------------------
# Non-Linux: explain and stop.
# ---------------------------------------------------------------------------
OS="$(uname -s)"
if [[ "$OS" != "Linux" ]]; then
  case "$MODE" in
    dry-run|status)
      printf '%s: this host is %s, not Linux. Host tuning (governor, IRQ affinity,\n' "$PROG" "$OS"
      printf 'huge pages, isolcpus, ...) only applies to the Linux benchmark host (§18).\n'
      printf 'Nothing was inspected or changed. Use this machine for development and unit tests.\n'
      exit 0 ;;
    *)
      printf '%s: refusing --%s on %s: this script only tunes Linux hosts.\n' "$PROG" "$MODE" "$OS" >&2
      exit 1 ;;
  esac
fi

IS_ROOT=0
[[ "$(id -u)" == "0" ]] && IS_ROOT=1
if [[ "$MODE" == "apply" || "$MODE" == "revert" ]] && (( ! IS_ROOT )); then
  printf '%s: --%s must run as root (try: sudo %s --%s ...)\n' "$PROG" "$MODE" "$0" "$MODE" >&2
  exit 2
fi

SYS_CPU=/sys/devices/system/cpu

# ---------------------------------------------------------------------------
# Online CPUs, reserved and housekeeping sets.
# ---------------------------------------------------------------------------
if [[ -r "$SYS_CPU/online" ]]; then
  ONLINE="$(expand_list "$(cat "$SYS_CPU/online")")"
else
  ONLINE="$(expand_list "0-$(( $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1) - 1 ))")"
fi

SMT_CTRL_FILE="$SYS_CPU/smt/control"
SMT_CTRL="$(read_val "$SMT_CTRL_FILE" 2>/dev/null || true)"

# CPUs that stay online after SMT is turned off: the first thread of each core.
EFFECTIVE="$ONLINE"
if (( SMT_OFF )) && [[ "$SMT_CTRL" == "on" ]]; then
  EFFECTIVE=""
  for c in $ONLINE; do
    sib="$(cat "$SYS_CPU/cpu$c/topology/thread_siblings_list" 2>/dev/null || echo "$c")"
    first="$(expand_list "$sib")"
    first="${first%% *}"
    [[ "$first" == "$c" ]] && EFFECTIVE="$EFFECTIVE $c"
  done
  EFFECTIVE="${EFFECTIVE# }"
fi

RESERVED=""
HOUSEKEEPING=""
if [[ -n "$CPUS_ARG" ]]; then
  RESERVED="$(expand_list "$CPUS_ARG")"
  [[ -n "$RESERVED" ]] || die_usage "--cpus '$CPUS_ARG' contains no CPUs (is a range reversed?)"
  for c in $RESERVED; do
    in_list "$c" "$ONLINE" || die_usage "--cpus: CPU $c is not online (online: $(compress_list "$ONLINE"))"
    if ! in_list "$c" "$EFFECTIVE"; then
      die_usage "--cpus: CPU $c is an SMT sibling that --smt-off would take offline (kept CPUs: $(compress_list "$EFFECTIVE"))"
    fi
  done
  if in_list 0 "$RESERVED" && (( ! ALLOW_CPU0 )); then
    die_usage "--cpus includes CPU 0, which runs housekeeping work; pass --allow-cpu0 to permit it"
  fi
  HOUSEKEEPING="$(list_minus "$EFFECTIVE" "$RESERVED")"
  [[ -n "$HOUSEKEEPING" ]] || die_usage "--cpus reserves every online CPU; at least one must remain for IRQs and the OS"
fi
RESERVED_STR="$(compress_list "$RESERVED")"
HOUSEKEEPING_STR="$(compress_list "$HOUSEKEEPING")"

# ---------------------------------------------------------------------------
# State file
# ---------------------------------------------------------------------------
# Format: one record per line, tab-separated:  KIND <TAB> KEY <TAB> VALUE
#   F  /path/to/file  original-value     (a sysfs/procfs value we changed)
#   S  irqbalance     active             (a service we stopped)
state_save() {
  { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$STATE_FILE"; } 2>/dev/null
}

state_init() {
  if [[ -e "$STATE_FILE" ]]; then
    if (( ! FORCE )); then
      printf '%s: state file %s already exists: a previous --apply has not been reverted.\n' "$PROG" "$STATE_FILE" >&2
      printf 'Run --revert first, or pass --force to replace it (it is kept as %s.prev).\n' "$STATE_FILE" >&2
      exit 1
    fi
    mv -f "$STATE_FILE" "$STATE_FILE.prev" || { printf '%s: cannot move %s aside\n' "$PROG" "$STATE_FILE" >&2; exit 3; }
    warn "--force: previous state file moved to $STATE_FILE.prev"
  fi
  local old_umask
  old_umask="$(umask)"
  umask 077
  if ! { printf '# adsb tune-host state v1 created %s cpus=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$RESERVED_STR" > "$STATE_FILE"; } 2>/dev/null; then
    umask "$old_umask"
    printf '%s: cannot create state file %s; nothing was changed.\n' "$PROG" "$STATE_FILE" >&2
    exit 3
  fi
  umask "$old_umask"
  info "originals are saved to $STATE_FILE before each change"
}

# ---------------------------------------------------------------------------
# The one place that changes a value (dry-run prints the command instead).
#   set_value PATH NEW LABEL [irq]
# ---------------------------------------------------------------------------
set_value() {
  local path="$1" new="$2" label="$3" kind="${4:-}" cur err
  if [[ ! -e "$path" ]]; then
    skip "$label: $path not present"
    return 0
  fi
  if ! cur="$(read_val "$path")" || [[ -z "$cur" ]]; then
    warn "$label: cannot read $path; left alone"
    return 0
  fi
  if [[ "$cur" == "$new" ]]; then
    ok "$label: already $new"
    return 0
  fi
  if [[ "$MODE" == "dry-run" ]]; then
    would "echo $new > $path    # $label, currently $cur"
    return 0
  fi
  if ! state_save F "$path" "$cur"; then
    fail "$label: could not record the original value in $STATE_FILE; not changed"
    record_failure "$label ($path): state file write failed"
    return 1
  fi
  if err="$(write_file "$path" "$new")"; then
    done_ "$label: $cur -> $new"
    N_CHANGED=$((N_CHANGED + 1))
    return 0
  fi
  if [[ "$kind" == "irq" && "$err" != *"Read-only"* && "$err" != *"Permission denied"* ]]; then
    N_REFUSED=$((N_REFUSED + 1))
    REFUSED_LIST="$REFUSED_LIST ${label#IRQ }"
    return 0
  fi
  fail "$label: echo $new > $path: $err"
  record_failure "$label ($path): $err"
  return 1
}

# ---------------------------------------------------------------------------
# Individual tasks
# ---------------------------------------------------------------------------
task_smt() {
  section "SMT"
  if [[ ! -e "$SMT_CTRL_FILE" ]]; then
    skip "SMT control: $SMT_CTRL_FILE not present"
    return
  fi
  if (( ! SMT_OFF )); then
    info "SMT control is '$SMT_CTRL' (pass --smt-off to disable it for benchmark runs, §18)"
    return
  fi
  case "$SMT_CTRL" in
    on|off) set_value "$SMT_CTRL_FILE" off "SMT control" ;;
    *) skip "SMT control is '$SMT_CTRL'; cannot be changed at runtime" ;;
  esac
}

task_turbo() {
  section "Turbo / boost"
  local f=""
  if [[ -e "$SYS_CPU/intel_pstate/no_turbo" ]]; then
    f="$SYS_CPU/intel_pstate/no_turbo"
  elif [[ -e "$SYS_CPU/cpufreq/boost" ]]; then
    f="$SYS_CPU/cpufreq/boost"
  fi
  if [[ -z "$f" ]]; then
    skip "turbo: neither intel_pstate/no_turbo nor cpufreq/boost present"
    return
  fi
  if (( ! NO_TURBO )); then
    info "turbo: $f = $(read_val "$f") (pass --no-turbo to disable it for stable frequency)"
    return
  fi
  if [[ "$f" == */no_turbo ]]; then
    set_value "$f" 1 "turbo (intel_pstate/no_turbo)"
  else
    set_value "$f" 0 "boost (cpufreq/boost)"
  fi
}

# Online CPUs that have a cpufreq governor file (re-read at call time so it
# reflects SMT changes made earlier in --apply).
governor_files() {
  local f c on
  for f in "$SYS_CPU"/cpu[0-9]*/cpufreq/scaling_governor; do
    [[ -e "$f" ]] || continue
    c="${f#"$SYS_CPU"/cpu}"; c="${c%%/*}"
    on="$(cat "$SYS_CPU/cpu$c/online" 2>/dev/null || echo 1)"
    [[ "$on" == "1" ]] && printf '%s\n' "$f"
  done
}

task_governor() {
  section "CPU frequency governor"
  local files f c avail
  files="$(governor_files)"
  if [[ -z "$files" ]]; then
    skip "governor: no $SYS_CPU/cpu*/cpufreq/scaling_governor (VM or fixed frequency)"
    return
  fi
  for f in $files; do
    c="${f#"$SYS_CPU"/}"; c="${c%%/*}"
    avail="$(cat "${f%/*}/scaling_available_governors" 2>/dev/null || echo performance)"
    if ! in_list performance "$avail"; then
      warn "governor $c: 'performance' not offered (available: $avail); left alone"
      continue
    fi
    set_value "$f" performance "governor $c"
  done
}

irqbalance_state() {
  # Echo: systemd-active | running | inactive
  if have systemctl && systemctl is-active --quiet irqbalance 2>/dev/null; then
    echo systemd-active; return
  fi
  local f comm
  for f in /proc/[0-9]*/comm; do
    comm="$(cat "$f" 2>/dev/null)" || continue
    [[ "$comm" == "irqbalance" ]] && { echo running; return; }
  done
  echo inactive
}

task_irqbalance() {
  section "irqbalance"
  case "$(irqbalance_state)" in
    systemd-active)
      if [[ "$MODE" == "dry-run" ]]; then
        would "systemctl stop irqbalance    # it would undo IRQ affinity; --revert restarts it"
      elif state_save S irqbalance active; then
        if systemctl stop irqbalance 2>/dev/null; then
          done_ "irqbalance: stopped (it is still enabled at boot; --revert restarts it)"
          N_CHANGED=$((N_CHANGED + 1))
        else
          fail "irqbalance: systemctl stop irqbalance failed"
          record_failure "irqbalance: systemctl stop failed"
        fi
      else
        fail "irqbalance: could not record state in $STATE_FILE; not stopped"
        record_failure "irqbalance: state file write failed"
      fi ;;
    running)
      warn "irqbalance is running but not as an active systemd unit; this script will not kill it."
      warn "Stop it yourself, or it will move IRQs back onto the reserved cores." ;;
    *)
      ok "irqbalance: not running" ;;
  esac
}

task_irqs() {
  section "IRQ affinity"
  if [[ -z "$RESERVED" ]]; then
    skip "IRQ affinity: no --cpus given, nothing to steer away from"
    return
  fi
  info "reserved cores: $RESERVED_STR   housekeeping cores: $HOUSEKEEPING_STR"
  local f irq cur cur_list new_list n_total=0 n_ok=0
  for irq in $(irq_numbers); do
    f="/proc/irq/$irq/smp_affinity_list"
    n_total=$((n_total + 1))
    cur="$(read_val "$f")" || continue
    cur_list="$(expand_list "$cur")"
    if ! list_intersects "$cur_list" "$RESERVED"; then
      n_ok=$((n_ok + 1))
      continue
    fi
    new_list="$(list_minus "$cur_list" "$RESERVED")"
    # Drop CPUs that will not be online (e.g. SMT siblings with --smt-off).
    new_list="$(list_minus "$new_list" "$(list_minus "$ONLINE" "$EFFECTIVE")")"
    [[ -n "$new_list" ]] || new_list="$HOUSEKEEPING"
    set_value "$f" "$(compress_list "$new_list")" "IRQ $irq" irq
  done
  if (( n_total == 0 )); then
    skip "IRQ affinity: no /proc/irq/*/smp_affinity_list files"
  else
    ok "$n_ok of $n_total IRQs already avoid the reserved cores"
  fi
  if (( N_REFUSED > 0 )); then
    warn "$N_REFUSED IRQs refused the write (kernel-managed or per-CPU; skipped):$REFUSED_LIST"
    warn "Boot with isolcpus=managed_irq,... (below) to keep managed IRQs off the reserved cores."
  fi

  local dfile=/proc/irq/default_smp_affinity dcur dlist dnew
  if [[ ! -e "$dfile" ]]; then
    skip "default IRQ affinity: $dfile not present"
    return
  fi
  dcur="$(read_val "$dfile")"
  dlist="$(mask_to_list "$dcur")"
  dlist="$(list_minus "$dlist" "$(list_minus "$ONLINE" "$EFFECTIVE")")"
  if [[ -n "$dcur" ]] && ! list_intersects "$dlist" "$RESERVED"; then
    ok "default IRQ affinity: already $dcur (CPUs $(compress_list "$dlist"))"
    return
  fi
  dnew="$(list_minus "$dlist" "$RESERVED")"
  [[ -n "$dnew" ]] || dnew="$HOUSEKEEPING"
  set_value "$dfile" "$(list_to_mask "$dnew")" "default IRQ affinity (CPUs $(compress_list "$dnew"))"
}

task_memory() {
  section "Huge pages"
  local f=/proc/sys/vm/nr_hugepages cur got
  if [[ -e "$f" ]]; then
    cur="$(read_val "$f")"
    if [[ "$cur" =~ ^[0-9]+$ ]] && (( cur >= HUGEPAGES )); then
      ok "vm.nr_hugepages: $cur reserved (>= $HUGEPAGES requested; never lowered)"
    else
      set_value "$f" "$HUGEPAGES" "vm.nr_hugepages"
      if [[ "$MODE" == "apply" ]]; then
        got="$(read_val "$f")"
        if [[ "$got" =~ ^[0-9]+$ ]] && (( got < HUGEPAGES )); then
          warn "kernel reserved only $got of $HUGEPAGES 2 MB pages (memory fragmented); add hugepages=$HUGEPAGES to the kernel command line for a boot-time reservation"
        fi
      fi
    fi
  else
    skip "vm.nr_hugepages: $f not present"
  fi
  set_value /sys/kernel/mm/transparent_hugepage/enabled madvise "transparent hugepages"
}

task_sysctls() {
  section "Scheduler / kernel sysctls"
  set_value /proc/sys/kernel/timer_migration 0 "kernel.timer_migration"
  set_value /proc/sys/vm/stat_interval "$STAT_INTERVAL" "vm.stat_interval"
  local f=/proc/sys/kernel/perf_event_paranoid cur
  if [[ -e "$f" ]]; then
    cur="$(read_val "$f")"
    if [[ "$cur" =~ ^-?[0-9]+$ ]] && (( cur <= 1 )); then
      ok "kernel.perf_event_paranoid: already $cur (<= 1, perf usable; left alone)"
    else
      set_value "$f" 1 "kernel.perf_event_paranoid"
    fi
  else
    skip "kernel.perf_event_paranoid: $f not present"
  fi
  f=/proc/sys/kernel/sched_rt_runtime_us
  if [[ -e "$f" ]]; then
    cur="$(read_val "$f")"
    if [[ "$cur" == "-1" ]]; then
      info "kernel.sched_rt_runtime_us = -1 (RT throttling off; not changed by this script)"
    else
      info "kernel.sched_rt_runtime_us = $cur (RT tasks throttled to this per period; not changed by this script)"
    fi
  fi
}

task_cmdline() {
  section "Boot-time kernel command line (not changed by this script)"
  if [[ -z "$RESERVED" ]]; then
    skip "pass --cpus LIST to get the isolcpus/nohz_full/rcu_nocbs/irqaffinity line"
    return
  fi
  local cmdline want tok key have_tok missing=""
  cmdline=" $(cat /proc/cmdline 2>/dev/null) "
  want="isolcpus=managed_irq,domain,$RESERVED_STR nohz_full=$RESERVED_STR rcu_nocbs=$RESERVED_STR irqaffinity=$HOUSEKEEPING_STR"
  for tok in $want; do
    key="${tok%%=*}"
    if [[ "$cmdline" == *" $tok "* ]]; then
      ok "/proc/cmdline has $tok"
      continue
    fi
    have_tok="$(tr ' ' '\n' <<<"$cmdline" | grep -m1 "^${key}=" || true)"
    if [[ -n "$have_tok" ]]; then
      warn "/proc/cmdline has $have_tok, want $tok"
    else
      warn "/proc/cmdline lacks $tok"
    fi
    missing="$missing $tok"
  done
  if [[ -n "$missing" ]]; then
    info "Add to the kernel command line (e.g. GRUB_CMDLINE_LINUX in /etc/default/grub, then"
    info "update-grub or grub2-mkconfig, then reboot; on a Pi edit /boot/firmware/cmdline.txt):"
    printf '\n    %s\n\n' "$want"
    info "nohz_full needs CONFIG_NO_HZ_FULL; isolcpus=managed_irq keeps managed IRQs off these cores."
  fi
}

# ---------------------------------------------------------------------------
# --status
# ---------------------------------------------------------------------------
do_status() {
  printf 'Host tuning status: %s (%s)\n' "$(hostname)" "$(uname -r)"
  section "CPUs"
  info "online: $(compress_list "$ONLINE")"
  [[ -n "$RESERVED" ]] && info "reserved (--cpus): $RESERVED_STR   housekeeping: $HOUSEKEEPING_STR"
  if [[ -e "$SMT_CTRL_FILE" ]]; then
    info "smt/control = $SMT_CTRL   smt/active = $(read_val "$SYS_CPU/smt/active")"
  else
    skip "smt/control not present"
  fi
  if [[ -e "$SYS_CPU/intel_pstate/no_turbo" ]]; then
    info "intel_pstate/no_turbo = $(read_val "$SYS_CPU/intel_pstate/no_turbo")"
  elif [[ -e "$SYS_CPU/cpufreq/boost" ]]; then
    info "cpufreq/boost = $(read_val "$SYS_CPU/cpufreq/boost")"
  else
    skip "turbo control not present"
  fi
  local files
  files="$(governor_files)"
  if [[ -n "$files" ]]; then
    # shellcheck disable=SC2086  # list of sysfs paths without spaces
    info "governors: $(cat $files 2>/dev/null | sort | uniq -c | awk '{printf "%s%s x%s", (NR>1?", ":""), $2, $1}')"
  else
    skip "no cpufreq governors (VM or fixed frequency)"
  fi

  section "IRQs"
  info "irqbalance: $(irqbalance_state)"
  local irq cur n_total=0 n_on=0 on_list=""
  for irq in $(irq_numbers); do
    n_total=$((n_total + 1))
    if [[ -n "$RESERVED" ]]; then
      cur="$(read_val "/proc/irq/$irq/smp_affinity_list")"
      if list_intersects "$(expand_list "$cur")" "$RESERVED"; then
        n_on=$((n_on + 1))
        on_list="$on_list $irq"
      fi
    fi
  done
  if [[ -n "$RESERVED" ]]; then
    if (( n_on == 0 )); then
      ok "$n_total IRQs, none may run on reserved cores $RESERVED_STR"
    else
      warn "$n_on of $n_total IRQs may run on reserved cores $RESERVED_STR:$on_list"
    fi
  else
    info "$n_total IRQs (pass --cpus to check them against the reserved cores)"
  fi
  if [[ -e /proc/irq/default_smp_affinity ]]; then
    cur="$(read_val /proc/irq/default_smp_affinity)"
    info "default_smp_affinity = $cur (CPUs $(compress_list "$(mask_to_list "$cur")"))"
  fi

  section "Memory"
  info "vm.nr_hugepages = $(read_val /proc/sys/vm/nr_hugepages 2>/dev/null || echo n/a)   $(awk '/HugePages_(Total|Free)/ {printf "%s %s  ", $1, $2}' /proc/meminfo 2>/dev/null)"
  info "transparent_hugepage/enabled = $(read_val /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)"

  section "Sysctls"
  local s
  for s in kernel/timer_migration vm/stat_interval kernel/perf_event_paranoid kernel/sched_rt_runtime_us; do
    if [[ -e "/proc/sys/$s" ]]; then
      info "${s//\//.} = $(read_val "/proc/sys/$s")"
    else
      skip "${s//\//.} not present"
    fi
  done

  task_cmdline

  section "State"
  if [[ -e "$STATE_FILE" ]]; then
    warn "$STATE_FILE exists ($(grep -vc '^#' "$STATE_FILE" 2>/dev/null) saved values): host is tuned; --revert restores it"
  else
    info "no state file at $STATE_FILE (nothing applied, or already reverted)"
  fi
}

# ---------------------------------------------------------------------------
# --revert
# ---------------------------------------------------------------------------
allowed_path() {
  case "$1" in
    /proc/irq/default_smp_affinity) return 0 ;;
    /proc/sys/vm/nr_hugepages|/proc/sys/vm/stat_interval) return 0 ;;
    /proc/sys/kernel/timer_migration|/proc/sys/kernel/perf_event_paranoid) return 0 ;;
    /sys/kernel/mm/transparent_hugepage/enabled) return 0 ;;
    "$SYS_CPU/smt/control"|"$SYS_CPU/intel_pstate/no_turbo"|"$SYS_CPU/cpufreq/boost") return 0 ;;
  esac
  [[ "$1" =~ ^/proc/irq/[0-9]+/smp_affinity_list$ ]] && return 0
  [[ "$1" =~ ^/sys/devices/system/cpu/cpu[0-9]+/cpufreq/scaling_governor$ ]] && return 0
  return 1
}

revert_one() {
  local path="$1" orig="$2" cur err
  if ! allowed_path "$path"; then
    fail "state file names $path, which this script never manages; ignored"
    record_failure "$path: not a managed path"
    return
  fi
  if [[ ! -e "$path" ]]; then
    skip "$path no longer exists (e.g. device removed); nothing to restore"
    return
  fi
  cur="$(read_val "$path")"
  if [[ "$path" == /proc/irq/default_smp_affinity ]]; then
    if [[ "$(mask_to_list "$cur")" == "$(mask_to_list "$orig")" ]]; then
      ok "$path: already $orig"
      return
    fi
  elif [[ "$cur" == "$orig" ]]; then
    ok "$path: already $orig (unchanged or never changed)"
    return
  fi
  if err="$(write_file "$path" "$orig")"; then
    done_ "$path: $cur -> $orig"
    N_CHANGED=$((N_CHANGED + 1))
  else
    fail "echo $orig > $path: $err"
    record_failure "$path: $err"
  fi
}

do_revert() {
  printf 'Reverting host tuning from %s\n' "$STATE_FILE"
  if [[ ! -e "$STATE_FILE" ]]; then
    printf '%s: no state file at %s; nothing to revert.\n' "$PROG" "$STATE_FILE" >&2
    exit 1
  fi
  # The state file drives root writes: insist it is root's and private.
  local owner perms
  owner="$(stat -c %u "$STATE_FILE" 2>/dev/null)"
  perms="$(stat -c %a "$STATE_FILE" 2>/dev/null)"
  if [[ "$owner" != "0" ]] || (( (8#${perms:-777} & 8#022) != 0 )); then
    printf '%s: refusing %s: it must be owned by root and not group/world-writable (owner=%s mode=%s)\n' \
      "$PROG" "$STATE_FILE" "${owner:-?}" "${perms:-?}" >&2
    exit 1
  fi

  local kinds=() keys=() vals=() kind key val i n
  while IFS=$'\t' read -r kind key val || [[ -n "$kind" ]]; do
    [[ -z "$kind" || "$kind" == \#* ]] && continue
    kinds+=("$kind"); keys+=("$key"); vals+=("$val")
  done < "$STATE_FILE"
  n=${#kinds[@]}
  info "$n saved records"

  # SMT first: original IRQ affinities may name sibling CPUs.
  section "SMT"
  for (( i = 0; i < n; i++ )); do
    [[ "${kinds[$i]}" == F && "${keys[$i]}" == "$SMT_CTRL_FILE" ]] && revert_one "${keys[$i]}" "${vals[$i]}"
  done
  section "Values (newest first)"
  for (( i = n - 1; i >= 0; i-- )); do
    [[ "${kinds[$i]}" == F && "${keys[$i]}" != "$SMT_CTRL_FILE" ]] && revert_one "${keys[$i]}" "${vals[$i]}"
  done
  section "Services"
  local any_svc=0
  for (( i = 0; i < n; i++ )); do
    [[ "${kinds[$i]}" == S ]] || continue
    any_svc=1
    if [[ "${keys[$i]}" != irqbalance ]]; then
      fail "state file names service ${keys[$i]}, which this script never manages; ignored"
      record_failure "service ${keys[$i]}: not managed"
      continue
    fi
    if systemctl start irqbalance 2>/dev/null; then
      done_ "irqbalance: restarted"
      N_CHANGED=$((N_CHANGED + 1))
    else
      fail "systemctl start irqbalance failed"
      record_failure "irqbalance: systemctl start failed"
    fi
  done
  (( any_svc )) || ok "no services to restart"

  printf '\n'
  if (( N_FAILED > 0 )); then
    printf 'Revert incomplete: %d restored, %d failed:\n%s' "$N_CHANGED" "$N_FAILED" "$FAILED_LIST"
    printf 'State file kept at %s; fix the cause and run --revert again.\n' "$STATE_FILE"
    exit 3
  fi
  rm -f "$STATE_FILE"
  printf 'Revert complete: %d values restored; %s removed.\n' "$N_CHANGED" "$STATE_FILE"
  exit 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
case "$MODE" in
  status) do_status; exit 0 ;;
  revert) do_revert ;;
esac

if [[ "$MODE" == "dry-run" ]]; then
  printf 'Dry run on %s: nothing will be changed. Lines marked [ do ] are the exact\n' "$(hostname)"
  printf 'commands --apply would run as root (originals saved to %s first).\n' "$STATE_FILE"
  [[ -z "$RESERVED" ]] && printf 'No --cpus given: IRQ affinity and boot-line steps are skipped.\n'
else
  printf 'Applying host tuning on %s (reserved cores %s)\n' "$(hostname)" "$RESERVED_STR"
  state_init
fi

task_smt        # first: it changes which CPUs are online
task_turbo
task_governor
task_irqbalance # before IRQs, so it cannot move them back
task_irqs
task_memory
task_sysctls
task_cmdline

printf '\n'
if [[ "$MODE" == "dry-run" ]]; then
  printf 'Dry run complete. Re-run with --apply (as root) to make these changes.\n'
  exit 0
fi

printf 'Apply summary: %d changed, %d failed' "$N_CHANGED" "$N_FAILED"
(( N_REFUSED > 0 )) && printf ', %d IRQs refused (skipped)' "$N_REFUSED"
printf '.\n'
if [[ "$(grep -vc '^#' "$STATE_FILE" 2>/dev/null)" == "0" ]]; then
  rm -f "$STATE_FILE"
  printf 'Nothing was changed; state file removed.\n'
else
  printf 'Originals saved in %s; undo with: %s --revert%s\n' "$STATE_FILE" "$0" \
    "$([[ "$STATE_FILE" != /var/tmp/adsb-tune-host.state ]] && printf ' --state %s' "$STATE_FILE")"
fi
if (( N_FAILED > 0 )); then
  printf 'Failed writes:\n%s' "$FAILED_LIST"
  exit 3
fi
exit 0
