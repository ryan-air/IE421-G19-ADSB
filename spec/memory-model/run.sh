#!/usr/bin/env bash
# Model-checks the two lock-free rings with GenMC (README_UPDATES.md §12):
# every correct model must verify, and every mutant (one memory ordering
# weakened, or one step of the protocol removed) must be caught. A mutant that
# verifies means the model has lost its teeth, and the run fails.
#
#   GENMC=/path/to/genmc bash spec/memory-model/run.sh
#
# Takes about 30 s. See README.md for what each model covers.
set -u
cd "$(dirname "$0")"
GENMC=${GENMC:-genmc}
if ! command -v "$GENMC" >/dev/null 2>&1; then
  echo "run.sh: GenMC not found (set GENMC=/path/to/genmc; see README.md)" >&2
  exit 2
fi

pass=0
fail=0
# check EXPECT NAME FILE [compiler flags...]; EXPECT is ok or error
check() {
  local expect=$1 name=$2 file=$3
  shift 3
  local out got
  out=$("$GENMC" --disable-estimation -- "$@" "$file" 2>&1)
  if grep -q "^No errors were detected" <<<"$out"; then
    got=ok
  elif grep -qE "^Error: (Non-atomic race|Safety violation)" <<<"$out"; then
    got=error
  else
    got=broken
  fi
  local what
  what=$(grep -oE "^(No errors were detected|Error: [A-Za-z -]+)" <<<"$out" | head -1)
  local n
  n=$(grep -oE "complete executions explored: [0-9]+" <<<"$out" | grep -oE "[0-9]+$")
  if [[ $got == "$expect" ]]; then
    pass=$((pass + 1))
    printf "ok    %-52s %s%s\n" "$name" "$what" "${n:+ ($n executions)}"
  else
    fail=$((fail + 1))
    printf "FAIL  %-52s expected %s, got: %s\n" "$name" "$expect" "${what:-$(tail -3 <<<"$out")}"
  fi
}
R=memory_order_relaxed

echo "== SPSC ring (spsc_ring.hpp): capacity 2, 4 messages"
for form in single batch; do
  flags=()
  [[ $form == batch ]] && flags=(-DBATCH)
  check ok "spsc $form" spsc_ring.c ${flags[@]+"${flags[@]}"}
  for m in PUSH_PUBLISH PUSH_SEE_READ POP_SEE_WRITE POP_FREE; do
    check error "spsc $form, mutant $m relaxed" spsc_ring.c ${flags[@]+"${flags[@]}"} -D$m=$R
  done
done

echo "== shm ring slot protocol (shm_ring.hpp), 1 writer, 1 reader"
for form in fence tsan; do
  flags=()
  [[ $form == tsan ]] && flags=(-DTSAN_FORM)
  check ok "shm $form form, 1 slot, 3 messages" shm_slot.c ${flags[@]+"${flags[@]}"} -DCAPACITY=1u -DMESSAGES=3u
  check ok "shm $form form, 2 slots, 4 messages" shm_slot.c ${flags[@]+"${flags[@]}"} -DCAPACITY=2u -DMESSAGES=4u
  check error "shm $form form, mutant SEQ_PUBLISH relaxed" shm_slot.c ${flags[@]+"${flags[@]}"} -DSEQ_PUBLISH=$R
  check error "shm $form form, mutant SEQ_SEE relaxed" shm_slot.c ${flags[@]+"${flags[@]}"} -DSEQ_SEE=$R
  check error "shm $form form, mutant no busy marker" shm_slot.c ${flags[@]+"${flags[@]}"} -DWRITE_BUSY=0
  check error "shm $form form, mutant no re-check after the copy" shm_slot.c ${flags[@]+"${flags[@]}"} -DRECHECK=0
done
check error "shm fence form, mutant no writer release fence" shm_slot.c -DWRITER_FENCE=0
check error "shm fence form, mutant no reader acquire fence" shm_slot.c -DREADER_FENCE=0
check error "shm tsan form, mutant payload stores relaxed" shm_slot.c -DTSAN_FORM -DPAYLOAD_STORE=$R
check error "shm tsan form, mutant payload loads relaxed" shm_slot.c -DTSAN_FORM -DPAYLOAD_LOAD=$R
# The assumption the model disproved: after a lap, next_seq() is past the
# lapped message. Kept as an expected counterexample (see resume_after_lap).
check error "shm: next_seq() after a lap can lag (resume_after_lap)" shm_slot.c -DCHECK_HEAD

echo
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
