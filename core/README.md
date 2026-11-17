# `core/` — Low-Latency Pipeline Foundations

C++20 building blocks for the pipeline described in [README_UPDATES.md](../README_UPDATES.md). This is the Week 1–2 "M0 foundations" work, the shared pieces every workstream depends on:

| Component | Header | What it is |
|---|---|---|
| SPSC ring | [`adsb/common/spsc_ring.hpp`](include/adsb/common/spsc_ring.hpp) | Lock-free single-producer/single-consumer ring: cache-line-separated indices, cached remote index, acquire/release only, batch push/pop, zero-copy `front()`/`pop()` |
| Latency histogram | [`adsb/common/histogram.hpp`](include/adsb/common/histogram.hpp) | Fixed-memory log-linear (HdrHistogram-style) histogram: 0.2% precision by default, exact min/max, nearest-rank percentiles, `merge()` across threads |
| Clocks | [`adsb/common/clock.hpp`](include/adsb/common/clock.hpp) | `mono_ns()`, `ticks()` (TSC / `CNTVCT_EL0`), and a fixed-point `TickConverter` calibrated against the monotonic clock |
| CRC-24 | [`adsb/modes/crc24.hpp`](include/adsb/modes/crc24.hpp) | Compile-time 1 KB table, frame syndrome, bit-by-bit reference implementation |
| Altitude / squawk | [`adsb/modes/altitude.hpp`](include/adsb/modes/altitude.hpp) | AC12/AC13 altitude (25 ft Q-bit and Gillham Gray code), ID13 → squawk |
| CPR | [`adsb/modes/cpr.hpp`](include/adsb/modes/cpr.hpp) | Global and local position decoding, airborne and surface, table-based NL (no trig on the hot path), encoder |
| Mode S decoder | [`adsb/modes/decoder.hpp`](include/adsb/modes/decoder.hpp) | Stateless frame → fixed-size `DecodedMsg`: DF0/4/5/11/16/17/18/20/21/24; ADS-B TC 0–22 and 28 |
| Beast parser | [`adsb/ingest/beast.hpp`](include/adsb/ingest/beast.hpp) | Resumable streaming parser for readsb/dump1090 Beast output (port 30005), handling escaping and resync, plus an encoder |
| Host check | [`scripts/check-host.sh`](scripts/check-host.sh) | Reports hardware timestamping, AF_XDP zero-copy drivers, Intel RDT/CAT, isolation settings, SMT, TSC, RTL-SDRs, and so on |

Nothing here allocates on the hot path, throws, or takes a lock.

## Build and test

Requires CMake ≥ 3.24 and a C++20 compiler (GCC ≥ 12, Clang ≥ 15, or Apple clang 15). GoogleTest and Google Benchmark are downloaded automatically at pinned versions and SHA-256 hashes.

```bash
cd core
cmake --preset debug && cmake --build --preset debug -j && ctest --preset debug
```

| Preset | Purpose |
|---|---|
| `debug` | Debug build + tests |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer + tests |
| `tsan` | ThreadSanitizer + tests (checks the SPSC ring's memory ordering) |
| `release` | Optimized build + tests |
| `bench` | `-O3`, native CPU, LTO, Google Benchmark binaries in `build/bench/bench/adsb_bench` |
| `fuzz` | libFuzzer targets (needs LLVM clang, not Apple clang) |
| `fuzz-standalone` | Same targets with a portable driver (corpus replay + seeded mutations); works on macOS |

Add `-G Ninja` to any configure command if Ninja is installed, and `-DADSB_WERROR=ON` to treat warnings as errors (CI does both).

### Fuzzing

```bash
# Linux, LLVM clang:
cmake --preset fuzz && cmake --build --preset fuzz
./build/fuzz/fuzz/fuzz_decoder -max_total_time=300 fuzz/corpus/decoder
./build/fuzz/fuzz/fuzz_beast   -max_total_time=300 fuzz/corpus/beast

# macOS / any compiler:
cmake --preset fuzz-standalone && cmake --build --preset fuzz-standalone
./build/fuzz-standalone/fuzz/fuzz_decoder -runs=1000000 -max_len=15 fuzz/corpus/decoder
./build/fuzz-standalone/fuzz/fuzz_beast   -runs=300000 fuzz/corpus/beast
```

Both harnesses check invariants beyond "doesn't crash":
- **Beast:** every emitted frame has the right length for its type, all input bytes are accounted for, and re-encoding the output then parsing it again reproduces the frames exactly.
- **Decoder:** a frame that fails CRC never reports fields, decoded values stay within physical ranges, and CPR decoding of the fields yields finite, in-range coordinates.

### Benchmarks

```bash
cmake --preset bench && cmake --build --preset bench
./build/bench/bench/adsb_bench
```

On Linux, pin to isolated cores for meaningful numbers (`taskset -c 2,3 ./build/bench/bench/adsb_bench`). Laptops and CI VMs give indicative numbers only.

### Host capability check

```bash
./scripts/check-host.sh          # human-readable
./scripts/check-host.sh --json   # for recording alongside benchmark results
```

Run it on every candidate benchmark host and receiver. It answers Week 1 open decisions 2 and 3 (benchmark host and NICs) in README_UPDATES.md §21.

## How correctness is checked

| Area | Evidence |
|---|---|
| CRC-24 | Published DF17 frames have zero syndrome. The table implementation matches the bit-by-bit polynomial division on random input. Every single-bit error in every test frame is detected. Address/Parity recovery works. |
| Decoder | Published vectors from *The 1090 MHz Riddle* and the pyModeS documentation: callsigns, altitudes, CPR fields, positions, ground speed, track, airspeed, heading, vertical rate, GNSS/baro difference. Synthetic frames cover DF0/4/5/11/16/18/20/21, TC28, and surface positions. Random valid frames stay in physical ranges. |
| CPR | Riddle worked example (global and local). pyModeS example. The NL table matches the closed-form formula over 250k latitudes. 100k random positions round-trip through encode → global and local decode within the ~5 m airborne resolution, surface within ~3 m. Pairs that straddle a zone boundary are rejected. |
| Altitude | Every 25 ft Q-bit code. An exhaustive Gillham check: all 1,280 valid codes decode to unique 100 ft altitudes from −1,200 to 126,700 ft, and adjacent altitudes differ by exactly one bit (the Gray-code property), so the decoder is checked without a hand-written encoder. |
| SPSC ring | Edge cases (full, empty, wrap, batch). Two-thread stress tests with 64-byte messages verify order and detect torn slots, at capacities 2–4096, under ThreadSanitizer. |
| Histogram | The bucket tiling is checked exhaustively (no gaps or overlaps), along with the relative-error bound for every bucket, percentiles against an exact sort on 200k log-normal samples, and merge equivalence. |
| Beast | Byte-at-a-time and random chunking of a 2,000-frame stream heavy with `0x1A`, plus garbage, unknown types, truncated frames, and reset. |

## Limits and next steps

- **Decoder gaps:** TC 29/31, Comm-B (BDS) registers, and single-bit error correction are not decoded yet. TC 20–22 GNSS height uses the same 25 ft encoding as readsb/dump1090; pyModeS treats it as metres. The planned readsb differential diff (README_UPDATES.md §7.3) will settle this against real traffic.
- **Address/Parity formats:** DF0/4/5/16/20/21/24 frames return `kAddressParity`. The track state engine must accept them only for ICAO addresses that are already live tracks.
- **Clock on ARM:** `ticks()` on ARM is the generic timer (24 MHz on Apple silicon, 54 MHz on the Pi 5), not CPU cycles. Resolution there is 18–42 ns.
- **Not yet built:** the track state engine, feed publisher, and recorder (Workstreams D, E, G) are the next pieces and will build on these headers.
