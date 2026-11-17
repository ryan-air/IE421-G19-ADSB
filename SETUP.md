# Team Setup Guide

How to get the new low-latency code (`core/`) building and tested on your machine. For *what* we are building and *why*, see [README_UPDATES.md](README_UPDATES.md). For the technical details of each component, see [core/README.md](core/README.md).

---

## 1. What exists today

The **M0 foundations** from the plan (README_UPDATES.md §19, Weeks 1–2) are implemented, tested, and running in CI:

| Piece | Location | Status |
|---|---|---|
| CMake project, presets, CI | `core/CMakeLists.txt`, `core/CMakePresets.json`, `.github/workflows/core.yml` | Done |
| Lock-free SPSC ring buffer | `core/include/adsb/common/spsc_ring.hpp` | Done; TSan-tested |
| Latency histogram + clocks | `core/include/adsb/common/histogram.hpp`, `clock.hpp` | Done |
| CRC-24 + Mode S / ADS-B decoder + CPR | `core/include/adsb/modes/`, `core/src/modes/` | Done; checked against published vectors |
| Beast protocol parser | `core/include/adsb/ingest/beast.hpp` | Done; fuzzed |
| Host capability check | `core/scripts/check-host.sh` | Done |

**Next up** (open for owners): track state engine (Workstream D), feed publisher (E), recorder/replay (G), and the IQ demodulator (A).

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

Expected result: `100% tests passed, 0 tests failed out of 75`.

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
| `release` | Optimized build + tests |
| `bench` | Microbenchmarks (`-O3`, native CPU, LTO) |
| `fuzz` | libFuzzer (Linux, LLVM clang) |
| `fuzz-standalone` | Fuzz targets with a portable driver (macOS, any compiler) |

Each preset builds into its own folder under `core/build/<preset>/`, so they never interfere with each other.

---

## 5. Fuzzing

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

## 6. Benchmarks

```bash
cmake --preset bench && cmake --build --preset bench -j
```

```bash
./build/bench/bench/adsb_bench
```

Laptop and VM numbers are only indicative. **Latency numbers for the report come only from the Linux benchmark host** with isolated cores (README_UPDATES.md §11.3), for example `taskset -c 2,3 ./build/bench/bench/adsb_bench`.

---

## 7. Everyone: run the host check and share the output

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

## 8. Testing on Linux from a Mac (Docker)

Some things (`tsan` on GCC, libFuzzer, `check-host.sh` Linux checks) only run on Linux. With Docker Desktop running, from the repository root:

```bash
docker run --rm -it -v "$PWD":/src:ro ubuntu:24.04 bash -c "apt-get update -qq && apt-get install -y -qq cmake ninja-build g++ ca-certificates >/dev/null && cmake -S /src/core -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DADSB_BUILD_TESTS=ON -DADSB_WERROR=ON && cmake --build /tmp/build && ctest --test-dir /tmp/build"
```

The source is mounted read-only and the build goes to `/tmp` inside the container, so it never touches your macOS build folders. A container is fine for correctness but not for latency measurements.

---

## 9. Working on the code

- **Branch per change** (`git checkout -b feature/track-state`), open a pull request, and let CI pass before merging.
- **New code goes in `core/`:** public headers in `include/adsb/<area>/`, sources in `src/<area>/`, tests in `tests/test_<name>.cpp`. Add new sources to the `adsb_core` target in `core/CMakeLists.txt` and new test files to `core/tests/CMakeLists.txt`.
- **Hot-path rules** (README_UPDATES.md §4): no allocation after startup, no locks, no exceptions, fixed-size POD messages, event time instead of wall time.
- **Every decoder change needs a test vector.** Published examples are best, otherwise synthetic frames with a corrected CRC (see `fix_parity` in `tests/test_decoder.cpp`).
- **Anything with threads or atomics must pass `tsan`.**

---

## 10. Troubleshooting

| Problem | Fix |
|---|---|
| `CMake 3.24 or higher is required` | Install a newer CMake (see §2) |
| Configure hangs or fails downloading GoogleTest | First configure needs internet; retry on a network that allows GitHub |
| `ThreadSanitizer: unexpected memory mapping` (or the ASan equivalent) on Linux | Kernel ASLR entropy too high for the sanitizer runtime: `sudo sysctl -w vm.mmap_rnd_bits=28` |
| `library 'libclang_rt.fuzzer_osx.a' not found` on macOS | Apple clang has no libFuzzer; use the `fuzz-standalone` preset |
| A preset fails after switching compilers | Delete that preset's folder (`rm -rf core/build/<preset>`) and configure again |
| `check-host.sh` says "Not Linux" | Expected on macOS: the Mac is for development, not latency numbers |
