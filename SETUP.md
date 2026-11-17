# Team Setup Guide

How to get the new low-latency code (`core/`) building and tested on your machine. For *what* we are building and *why*, see [README_UPDATES.md](README_UPDATES.md). For the technical details of each component, see [core/README.md](core/README.md).

---

## 1. What exists today

Implemented, tested, and running in CI (details in [core/README.md](core/README.md)):

| Piece | Location |
|---|---|
| CMake project, presets, CI | `core/CMakeLists.txt`, `core/CMakePresets.json`, `.github/workflows/core.yml` |
| Lock-free SPSC ring, latency histogram, clocks | `core/include/adsb/common/` |
| CRC-24, Mode S / ADS-B decoder **and encoders**, CPR | `core/include/adsb/modes/` |
| Beast protocol parser | `core/include/adsb/ingest/beast.hpp` |
| `.adsbrec` recording format | `core/include/adsb/record/` |
| Synthetic traffic generator (with ground truth) | `core/include/adsb/sim/`, tool `adsb-synth` |
| Track state engine + flat track table | `core/include/adsb/state/` |
| Replay (inline / two-thread pipeline), determinism check | `core/include/adsb/replay/`, tool `adsb-replay`, `core/scripts/check-determinism.sh` |
| Naive baseline build (mutex queues, `unordered_map`) | preset `naive-baseline` |
| Feed wire spec v0 + C++/JS/Python code generator | `protocol/` |
| TLA+ model of gap recovery, checked with TLC | `spec/` |
| Host capability check / host tuning | `core/scripts/check-host.sh`, `core/scripts/tune-host.sh` |

**Next up** (open for owners): live Beast TCP ingest, the multicast publisher with retransmit and snapshot servers (Workstream E), the WebSocket gateway, the A/B arbiter (B), and the IQ demodulator (A). The feed spec v0 also needs the team's review (§5 of `protocol/feed_spec.md`).

---

## 2. Install prerequisites

You need **CMake ≥ 3.24**, a **C++20 compiler**, and **git**. Ninja is optional but faster. The test and benchmark frameworks (GoogleTest, Google Benchmark) download automatically on first configure, so you need internet access the first time.

### macOS

```bash
xcode-select --install
```

```bash
brew install cmake ninja
```

Apple clang 15+ works for everything except libFuzzer (use the `fuzz-standalone` preset instead; see §5).

### Ubuntu 24.04 / Debian 12 / Raspberry Pi OS (Bookworm)

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git clang
```

**Ubuntu 22.04** ships CMake 3.22, which is too old. Install a newer one with `pip install --user cmake` (or from Kitware's apt repository), then check `cmake --version`.

### Windows

Use WSL2 with Ubuntu 24.04 and follow the Ubuntu steps. Native MSVC is not supported, because the code uses GCC/Clang builtins and POSIX clocks.

---

## 3. Clone, build, and run the tests

```bash
git clone https://github.com/ryan-air/IE421-G9-ADSB.git
```

```bash
cd IE421-G9-ADSB/core
```

```bash
cmake --preset debug && cmake --build --preset debug -j && ctest --preset debug
```

Expected result: `100% tests passed, 0 tests failed out of 131`.

The first configure takes a minute because it downloads the test frameworks. Later builds are incremental. Add `-G Ninja` after `--preset debug` on the first configure if you installed Ninja.

---

## 4. Before you push: sanitizers

CI runs every change with GCC and Clang under AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer, with warnings treated as errors. Run the same locally first so CI stays green:

```bash
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
```

```bash
cmake --preset tsan && cmake --build --preset tsan -j && ctest --preset tsan
```

To build with warnings as errors, the way CI does, add `-DADSB_WERROR=ON` to the configure step, for example `cmake --preset release -DADSB_WERROR=ON`.

### All presets

| Preset | Use it for |
|---|---|
| `debug` | Day-to-day development |
| `asan` | Memory errors and undefined behavior |
| `tsan` | Data races. **Required** for anything touching threads or atomics |
| `release` | Optimized build + tests + tools |
| `naive-baseline` | Same code with mutex queues and `std::unordered_map`, for before/after comparisons |
| `bench` | Microbenchmarks (`-O3`, native CPU, LTO) |
| `fuzz` | libFuzzer (Linux, LLVM clang) |
| `fuzz-standalone` | Fuzz targets with a portable driver (macOS, any compiler) |

Each preset builds into its own folder under `core/build/<preset>/`, so they never interfere with each other.

---

## 5. Synthetic traffic, replay, and the determinism check

No receiver is needed to work on the pipeline: generate traffic, then replay it.

```bash
cmake --preset release && cmake --build --preset release -j
```

```bash
./build/release/tools/adsb-synth -o /tmp/traffic.adsbrec --aircraft 200 --duration 120 --lines 2
```

```bash
./build/release/tools/adsb-replay /tmp/traffic.adsbrec
```

```bash
./build/release/tools/adsb-replay /tmp/traffic.adsbrec --pipeline --speed 20
```

The first replay prints counters (tracks, positions, duplicates, rejected frames) and a SHA-256 of everything the track engine emitted. The second runs the two-thread pipeline at 20× real time and prints latency percentiles. The same seed always produces the same recording, and the same recording always produces the same hash, at any speed and with or without `--pipeline`. If you change the engine and the hash changes, the output changed.

Before/after comparison and the full determinism check (CI runs this):

```bash
cmake --preset naive-baseline && cmake --build --preset naive-baseline -j
```

```bash
./scripts/check-determinism.sh build/release build/naive-baseline
```

---

## 6. Feed protocol and TLA+ model

The feed's wire format is defined once in `protocol/schema/feed_v0.json`. After editing it, regenerate the C++, JavaScript, and Python code and run the cross-language tests, from the repository root:

```bash
python3 protocol/gen.py
```

```bash
python3 -m unittest discover -s protocol/tests && node protocol/tests/test_feed.mjs
```

The gap-recovery protocol is model-checked with TLA+ (needs Java 11+; the script downloads a pinned `tla2tools.jar`):

```bash
bash spec/run-tlc.sh
```

---

## 7. Fuzzing

**macOS (or any compiler):**

```bash
cmake --preset fuzz-standalone && cmake --build --preset fuzz-standalone -j
```

```bash
./build/fuzz-standalone/fuzz/fuzz_decoder -runs=1000000 -max_len=15 fuzz/corpus/decoder
```

```bash
./build/fuzz-standalone/fuzz/fuzz_beast -runs=300000 fuzz/corpus/beast
```

**Linux with LLVM clang (real coverage-guided fuzzing):**

```bash
cmake --preset fuzz && cmake --build --preset fuzz -j
```

```bash
./build/fuzz/fuzz/fuzz_decoder -max_total_time=300 fuzz/corpus/decoder
```

If a fuzzer finds a crash, it writes a `crash-*` file into the current directory. Add that input as a unit test when you fix the bug.

---

## 8. Benchmarks

```bash
cmake --preset bench && cmake --build --preset bench -j
```

```bash
./build/bench/bench/adsb_bench
```

Laptop and VM numbers are only indicative. **Latency numbers for the report come only from the Linux benchmark host** with isolated cores (README_UPDATES.md §11.3). Prepare that host with `tune-host.sh` (dry-run by default; `--apply` needs root and is undone with `--revert`):

```bash
./scripts/tune-host.sh --dry-run --cpus 2-5
```

then pin the run, for example `taskset -c 2,3 ./build/bench/bench/adsb_bench` or `adsb-replay ... --cpus 2,3`.

---

## 9. Everyone: run the host check and share the output

This answers the Week 1 hardware questions (README_UPDATES.md §21) with data instead of guesses. Run it on every machine that might be a benchmark host, receiver, or camera node, including your laptop:

```bash
./scripts/check-host.sh
```

```bash
./scripts/check-host.sh --json > host-$(hostname).json
```

Post the JSON in the team channel. The things we most need to find:
- a Linux machine with **≥ 8 physical cores**, **Intel RDT (`cat_l3`)**, and root access (benchmark host);
- NICs reporting **`hw_timestamps=yes`** and **`af_xdp_zero_copy_driver=yes`** (wire-latency and kernel-bypass experiments);
- a Raspberry Pi 5 with an RTL-SDR (`rtl_sdr` row).

---

## 10. Testing on Linux from a Mac (Docker)

Some things (GCC builds, libFuzzer, the Linux checks in `check-host.sh`) only run on Linux. With Docker Desktop running, from the repository root:

```bash
docker run --rm -it -v "$PWD":/src:ro ubuntu:24.04 bash -c "apt-get update -qq && apt-get install -y -qq cmake ninja-build g++ ca-certificates >/dev/null && cmake -S /src/core -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DADSB_BUILD_TESTS=ON -DADSB_WERROR=ON && cmake --build /tmp/build && ctest --test-dir /tmp/build"
```

The source is mounted read-only and the build goes to `/tmp` inside the container, so it never touches your macOS build folders. A container is fine for correctness but not for latency measurements.

---

## 11. Working on the code

- **Branch per change** (`git checkout -b feature/track-state`), open a pull request, and let CI pass before merging.
- **New code goes in `core/`:** public headers in `include/adsb/<area>/`, sources in `src/<area>/`, tests in `tests/test_<name>.cpp`. Add new sources to the `adsb_core` target in `core/CMakeLists.txt` and new test files to `core/tests/CMakeLists.txt`.
- **Hot-path rules** (README_UPDATES.md §4): no allocation after startup, no locks, no exceptions, fixed-size POD messages, event time instead of wall time.
- **Every decoder change needs a test vector.** Published examples are best, otherwise synthetic frames with a corrected CRC (see `fix_parity` in `tests/test_decoder.cpp`).
- **Anything with threads or atomics must pass `tsan`.**
- **Track engine changes:** run `scripts/check-determinism.sh` and the `EndToEnd` tests, which compare engine output against the synthetic generator's ground truth.
- **Feed format changes:** edit `protocol/schema/feed_v0.json`, never the generated files, then run `python3 protocol/gen.py`.

---

## 12. Troubleshooting

| Problem | Fix |
|---|---|
| `CMake 3.24 or higher is required` | Install a newer CMake (see §2) |
| Configure hangs or fails downloading GoogleTest | First configure needs internet; retry on a network that allows GitHub |
| `ThreadSanitizer: unexpected memory mapping` (or the ASan equivalent) on Linux | Kernel ASLR entropy too high for the sanitizer runtime: `sudo sysctl -w vm.mmap_rnd_bits=28` |
| `library 'libclang_rt.fuzzer_osx.a' not found` on macOS | Apple clang has no libFuzzer; use the `fuzz-standalone` preset |
| A preset fails after switching compilers | Delete that preset's folder (`rm -rf core/build/<preset>`) and configure again |
| `check-host.sh` says "Not Linux" | Expected on macOS: the Mac is for development, not latency numbers |
