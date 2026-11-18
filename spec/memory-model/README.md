# Memory-model verification of the lock-free rings

Plan §12: the two lock-free structures on the hot path are checked against
the C++ memory model, not just tested on one machine.

| Structure | Code | Model |
|---|---|---|
| SPSC queue between pipeline stages | `core/include/adsb/common/spsc_ring.hpp` | `spsc_ring.c` |
| Shared-memory broadcast ring, slot seqlock | `core/include/adsb/feed/shm_ring.hpp` (`RingView::write` / `read`) | `shm_slot.c` |

Three layers, each catching what the one before cannot:

1. **ThreadSanitizer** (`ctest --preset tsan`; CI `gcc / tsan`, `clang / tsan`):
   races in the real code, on the interleavings that happen to run. TSan does
   not model `atomic_thread_fence`, so the shm ring's TSan build compiles the
   fence-free form (see `shm_ring.hpp`); the fence form is covered by layer 3.
2. **Stress runs on real hardware** (`adsb-ring-stress`; CI `ring stress` on
   x86 and ARM, 60 s each): billions of operations with jitter, every message
   checked for order, exactly-once delivery, and torn contents.
3. **GenMC model checking** (`run.sh`; CI `memory-model`): every execution the
   C11 memory model (RC11) allows, for a small configuration, of a line-for-line
   C transcription of each protocol.

## GenMC models

`spsc_ring.c` (`-DBATCH`: `try_push_n` / `try_pop_n`): capacity 2, four
messages, so the ring is full, empty, and wraps. Slots are plain memory as in
the C++ ring, so any missing ordering is a **data race** GenMC reports; the
consumer also asserts order and intact contents.

`shm_slot.c` (default: the fence form normal builds compile; `-DTSAN_FORM`: the
fence-free form): one writer laps one reader (1 slot / 3 messages, 2 slots / 4
messages). The reader asserts that every message it accepts is intact (no
word from another lap), that messages come in order, and resumes after a lap
the way `RingView::resume_after_lap` does. More readers add nothing (readers
never write shared memory) and multiply the state space.

**Mutants.** Each ordering, and each step of the slot protocol, is a macro.
`run.sh` checks the six correct models and 20 mutants (every acquire or release
weakened to relaxed, the busy marker removed, the re-check after the copy
removed, each fence removed); every mutant must be caught, or the run fails.
That is what shows the models are strong enough to find a real bug. The stress
test could not catch one of them (the SPSC consumer freeing a slot with a
relaxed store, a load-store reordering Apple's cores rarely perform); GenMC
catches it in the first execution.

**Finding.** The first run of `shm_slot.c` disproved an assumption in the
callers: after `read()` reports a lap, `next_seq()` is not necessarily past the
lapped message. A lap seen through the writer's busy marker (or the re-check
after the copy) does not synchronize with the writer's `next_seq` store, so a
reader can see the lap and still read an old head, even one below the message
it expected. `read()` itself was always correct (it never returned a torn or
wrong message), but `adsb-shm-bench`, two tests, and the stress tool resumed
at `next_seq()` and could step backwards (the stress tool even counted it as a
violation). Fixed with `RingView::resume_after_lap(seq)` (never below
`seq + 1`) and a unit test of that state; `run.sh` keeps the counterexample as
an expected failure (`-DCHECK_HEAD`). `ShmSubscriber` was not affected: after a
lap it resynchronizes from a snapshot, and a stale `oldest()` only causes one
more lap and resync.

**Limits.** The models are transcriptions: a change to either ring must be
mirrored here (the comments name the C++ line each step comes from). They
check small bounds (GenMC is exhaustive within them, not beyond), and RC11,
which is the language model: the standard compilation of C11 atomics to x86-TSO
and ARMv8 is proven sound for RC11, so (given a correct compiler) a protocol
correct under RC11 is correct on both. Message lengths up to 64 bytes are modelled as
two words plus the length.

## Running

GenMC needs LLVM 19 (or 20–22) on Linux. It is not on macOS; run it in Docker:

```bash
docker run --rm -v "$PWD":/src:ro ubuntu:24.04 bash -c \
  "cp -r /src/spec /tmp/spec && G=\$(bash /tmp/spec/memory-model/build-genmc.sh) && GENMC=\$G bash /tmp/spec/memory-model/run.sh"
```

On Ubuntu 24.04 directly: `GENMC=$(bash spec/memory-model/build-genmc.sh) bash spec/memory-model/run.sh`.
The build takes about 10 minutes; `run.sh` about 15–30 seconds.

Stress, as long as you like (do it on ARM: the benchmark box, a Graviton or
Ampere VM, an Apple machine):

```bash
core/build/release/tools/adsb-ring-stress --seconds 3600 --seed 7
```
