# `core/` — Low-Latency Pipeline

C++20 implementation of the pipeline described in [README_UPDATES.md](../README_UPDATES.md). For first-time setup see [SETUP.md](../SETUP.md).

## What is here

### Foundations (M0)

| Component | Header | What it is |
|---|---|---|
| SPSC ring | [`adsb/common/spsc_ring.hpp`](include/adsb/common/spsc_ring.hpp) | Lock-free single-producer/single-consumer ring: cache-line-separated indices, cached remote index, acquire/release only, batch push/pop, zero-copy `front()`/`pop()` |
| Latency histogram | [`adsb/common/histogram.hpp`](include/adsb/common/histogram.hpp) | Fixed-memory log-linear (HdrHistogram-style) histogram: 0.2% precision, exact min/max, nearest-rank percentiles, `merge()` |
| Clocks | [`adsb/common/clock.hpp`](include/adsb/common/clock.hpp) | `mono_ns()`, `ticks()` (TSC / `CNTVCT_EL0`), fixed-point `TickConverter` |
| CRC-24 | [`adsb/modes/crc24.hpp`](include/adsb/modes/crc24.hpp) | Compile-time 1 KB table, frame syndrome, bitwise reference |
| Altitude / squawk | [`adsb/modes/altitude.hpp`](include/adsb/modes/altitude.hpp) | AC12/AC13 (Q-bit and Gillham) decode and encode, ID13 ↔ squawk |
| CPR | [`adsb/modes/cpr.hpp`](include/adsb/modes/cpr.hpp) | Global and local decoding, airborne and surface, table NL, encoder |
| Mode S decoder | [`adsb/modes/decoder.hpp`](include/adsb/modes/decoder.hpp) | Frame → fixed-size `DecodedMsg`: DF0/4/5/11/16/17/18/20/21/24, ADS-B TC 0–22 and 28 |
| Beast parser | [`adsb/ingest/beast.hpp`](include/adsb/ingest/beast.hpp) | Resumable Beast (port 30005) parser and encoder |
| Live Beast ingest | [`adsb/ingest/beast_tcp.hpp`](include/adsb/ingest/beast_tcp.hpp) | TCP client for readsb/dump1090 Beast output: frames stamped with receive time, automatic reconnect with exponential backoff, `poll()` or busy-poll receive |

### Data path (M1–M4 groundwork)

| Component | Header | What it is |
|---|---|---|
| Frame encoders | [`adsb/modes/encoder.hpp`](include/adsb/modes/encoder.hpp) | The decoder's inverse: identification, airborne/surface position, velocity, DF11, DF4, DF5, with correct parity. Reproduces the published example frames byte for byte. |
| Recording format | [`adsb/record/recording.hpp`](include/adsb/record/recording.hpp) | `.adsbrec`: header with receiver position, length-checked records, CRC-32 footer, mmap reader, recovery of truncated files |
| Synthetic traffic | [`adsb/sim/synth.hpp`](include/adsb/sim/synth.hpp) | Aircraft on realistic paths around a receiver: DO-260B-like message rates, range-dependent loss, A/B receiver lines, corruption, per-line 12 MHz clocks with drift, and **ground truth** |
| Track store | [`adsb/state/track_store.hpp`](include/adsb/state/track_store.hpp) | Open-addressing table (Fibonacci hash, backward-shift deletion) with a 64-byte hot record; `std::unordered_map` baseline with the same interface |
| Track engine | [`adsb/state/track_engine.hpp`](include/adsb/state/track_engine.hpp) | CPR pairing, Address/Parity validation, duplicate filter, per-field out-of-order rejection, plausibility check with re-anchoring, timing-wheel expiry, `TrackUpdate`/`TrackDelete` output |
| Feed messages | [`adsb/protocol/feed_messages.hpp`](include/adsb/protocol/feed_messages.hpp) | **Generated** from [`protocol/schema/feed_v0.json`](../protocol/schema/feed_v0.json); see [`protocol/feed_spec.md`](../protocol/feed_spec.md) |
| MoldUDP64 framing | [`adsb/protocol/moldudp64.hpp`](include/adsb/protocol/moldudp64.hpp) | Packet builder, bounds-checked parser, subscriber `SequenceTracker` (gap / overlap / duplicate / heartbeat / new session) |
| Replay | [`adsb/replay/replay.hpp`](include/adsb/replay/replay.hpp) | Inline and two-thread pipeline replay at max or paced speed, per-stage latency histograms, SHA-256 of the output stream |
| Naive baseline | [`adsb/common/build_config.hpp`](include/adsb/common/build_config.hpp) | `-DADSB_NAIVE=ON` swaps `SpscRing` → mutex queue and the flat table → `std::unordered_map`. The pipeline code is identical. |

### Feed distribution (Workstream E, kernel UDP transport)

| Component | Header | What it is |
|---|---|---|
| Publisher | [`adsb/feed/publisher.hpp`](include/adsb/feed/publisher.hpp) | The track engine's sink. Sequences each message, packs MoldUDP64-style datagrams (immediate, or batched up to N µs), sends heartbeats when quiet, repeats end-of-session. Hands every message to the recovery thread through a lock-free SPSC log; no allocation or locks on the hot path. Includes deterministic loss injection (`LossyOut`). |
| Recovery servers | [`adsb/feed/recovery.hpp`](include/adsb/feed/recovery.hpp) | On their own thread: a ring of the last 65,536 messages for **retransmit** (unicast UDP; a header-only NAK when the range is gone) and the publisher's book for **snapshots** (TCP, non-blocking writes to up to 16 clients). The logic (`RecoveryState`) is separate from the sockets so tests drive it directly. |
| Subscriber | [`adsb/feed/subscriber.hpp`](include/adsb/feed/subscriber.hpp) | `SubscriberCore`: the recovery protocol of [`spec/FeedRecovery.tla`](../spec/FeedRecovery.tla) as code, plus sessions (publisher restarts, stale packets, late join). Buffers ahead of gaps, requests retransmits, falls back to snapshots. No sockets or clock of its own. `SubscriberClient` adds the sockets (multicast or unicast feed, request socket, blocking TCP snapshot fetch). |
| Book and snapshot stream | [`adsb/feed/book.hpp`](include/adsb/feed/book.hpp), [`adsb/feed/snapshot.hpp`](include/adsb/feed/snapshot.hpp) | The table a feed describes (ICAO → latest `TrackUpdate`) with a SHA-256 digest over its wire bytes, and the validated snapshot stream format |
| Sockets | [`adsb/feed/net.hpp`](include/adsb/feed/net.hpp) | IPv4 UDP multicast/unicast send and receive (group join, TTL 1, loopback), TCP listener |

### Tools and scripts

| Tool | Purpose |
|---|---|
| `build/<preset>/tools/adsb-synth` | Generate synthetic traffic: `.adsbrec`, plus optional raw Beast stream and ground-truth CSV |
| `build/<preset>/tools/adsb-replay` | Replay a recording through decode + track state: counters, output hash, optional pipeline latency |
| `build/<preset>/tools/adsb-live` | Run the pipeline on a live readsb feed (ingest → track state, optional recording, optional `--publish` to the feed), with receive → state latency |
| `build/<preset>/tools/adsb-publish` | Replay a recording through decode + track state and publish it as the feed, with the retransmit and snapshot servers; fault injection; prints the final book digest |
| `build/<preset>/tools/adsb-feed-cli` | Subscribe: verify sequencing, recover gaps, report counters and publish → applied latency, print the reconstructed book's digest |
| [`scripts/check-feed.sh`](scripts/check-feed.sh) | Feed integration test: loss, small ring, late join, publisher restart; every subscriber's book must equal the publisher's and a plain replay's (CI) |
| [`scripts/check-host.sh`](scripts/check-host.sh) | Report a machine's low-latency capabilities (NIC timestamps, AF_XDP, RDT, isolation, ...) |
| [`scripts/tune-host.sh`](scripts/tune-host.sh) | Put a Linux benchmark host into a reproducible tuned state (dry-run by default), and revert it |
| [`scripts/check-determinism.sh`](scripts/check-determinism.sh) | Prove the determinism contract: identical output across speeds, threading, and optimized vs naive builds |

Nothing on the hot path allocates, throws, or takes a lock (except deliberately, in the naive build).

## Build and test

```bash
cd core
cmake --preset debug && cmake --build --preset debug -j && ctest --preset debug
```

| Preset | Purpose |
|---|---|
| `debug` | Debug build + tests + tools |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer + tests |
| `tsan` | ThreadSanitizer + tests (SPSC ring, pipeline replay) |
| `release` | Optimized build + tests + tools |
| `naive-baseline` | Release build with mutex queues and `std::unordered_map` (+ tests, benchmarks) |
| `bench` | `-O3`, native CPU, LTO, Google Benchmark |
| `fuzz` / `fuzz-standalone` | Fuzz targets (`beast`, `decoder`, `packet`) with libFuzzer or the portable driver |

## Using the tools

```bash
# 2 minutes of traffic from 200 aircraft seen by two receivers (A/B lines)
./build/release/tools/adsb-synth -o /tmp/traffic.adsbrec --aircraft 200 --duration 120 --lines 2 --truth /tmp/truth.csv

# Replay as fast as possible: counters + output hash
./build/release/tools/adsb-replay /tmp/traffic.adsbrec

# Replay at 20x real time through the two-thread pipeline, with latency percentiles
./build/release/tools/adsb-replay /tmp/traffic.adsbrec --pipeline --speed 20

# Same recording through the naive baseline build, for a before/after comparison
./build/naive-baseline/tools/adsb-replay /tmp/traffic.adsbrec --pipeline --speed 20

# Live: connect to readsb's Beast output, track, and record (Ctrl-C to stop)
./build/release/tools/adsb-live --host 127.0.0.1 --port 30005 --record /tmp/live.adsbrec --receiver 40.0392,-88.2781

# Feed: publish a recording on multicast, with 1% injected loss; a subscriber reconstructs the book
./build/release/tools/adsb-feed-cli --feed 239.19.0.1:41900 --interface 127.0.0.1 --until-end &
./build/release/tools/adsb-publish /tmp/traffic.adsbrec --feed 239.19.0.1:41900 --interface 127.0.0.1 --speed 10 --drop-rate 0.01
#   both print the same "book digest" line

# Determinism contract (CI runs this too)
./scripts/check-determinism.sh build/release build/naive-baseline
```

At `--speed max` the reader outruns the worker, so "queue" latency mostly measures the backlog. Use a paced `--speed N` for latency numbers.

## Indicative numbers (Apple M2 laptop, not the benchmark host)

| Measurement | Result |
|---|---|
| CRC-24 syndrome | 8 ns |
| Decode one DF17 frame | 22 ns |
| CPR global decode | 19–30 ns |
| Track lookup, 4,000 aircraft | flat table **2.4 ns** vs `unordered_map` 8.1 ns |
| Track insert + erase churn | flat table **19–28 ns** vs `unordered_map` 190–210 ns |
| Full engine (decode + state) per frame, 300 aircraft | 118 ns flat vs 124 ns naive. At this size the table is not the bottleneck; decoding and CPR are. |
| Pipeline latency at 65k frames/s, frame in → state updated | optimized p50 0.38 µs / p99 1.8 µs / p99.9 21 µs; naive p50 1.2 µs / p99 29 µs / p99.9 830 µs |
| Feed, publish → applied in a subscriber on the same host (UDP loopback, 11k messages/s, no loss) | unicast p50 49 µs / p99 79 µs. Over macOS loopback **multicast** the p99 is about 26 ms: the macOS kernel occasionally holds multicast datagrams; unicast on the same machine does not. The p50 is mostly the subscriber's `poll()` wake-up. |

The Apple timer runs at 24 MHz, so latencies are quantized to about 42 ns. Laptops also have no core isolation. **Report numbers only from the Linux benchmark host** (README_UPDATES.md §11.3).

## How correctness is checked

| Area | Evidence |
|---|---|
| CRC-24 | Published frames have zero syndrome; the table matches bitwise division; every single-bit error is detected; Address/Parity recovery |
| Decoder | Published vectors from *The 1090 MHz Riddle* and pyModeS; synthetic frames for every DF; random valid frames stay in physical ranges |
| Encoders | Reproduce the published example frames exactly; round trips through the decoder over the full altitude, squawk, velocity, position, and surface-speed ranges |
| CPR | Riddle and pyModeS examples; NL table against the closed form; 100k random global/local round trips; zone-boundary rejection |
| Altitude | Exhaustive Gillham Gray-code property check over all 1,280 codes |
| SPSC ring / pipeline | Edge cases; two-thread stress with torn-slot detection; pipeline replay under ThreadSanitizer |
| Histogram | Exhaustive bucket tiling; error bound; percentiles against exact sort; every integer percentile of 1..100 |
| Beast | Every chunking of a stream heavy with `0x1A`; garbage, unknown types, truncation, reset |
| Recording | Round trip; empty file; corruption detected by CRC; truncated file recovers every complete record; bad records rejected |
| Track store | 300k-operation randomized differential test against `std::unordered_map` (including clustered keys); probe lengths at full load |
| Track engine | Unit tests for each rule. **End to end against ground truth:** 60 aircraft, 2 lines, 1% corrupted frames. Zero phantom tracks, every reported position within 12 m of the true transmitted position, callsigns/squawks/categories correct, duplicates filtered, every track expires. |
| Determinism | Same output hash across repeated runs, paced and max speed, inline and pipeline, optimized and naive builds (`scripts/check-determinism.sh`, CI) |
| Feed protocol | Golden packet decoded and re-encoded identically in C++, Python, and JavaScript; every truncation rejected; generated code checked for staleness in CI |
| Gap recovery | TLA+ model checked by TLC ([`spec/`](../spec/)); deliberately broken variant fails |
| Feed subscriber | A unit test for each rule (gap, overlap, duplicate, heartbeat tail, timeout retry, chunked requests, NAK, stale NAK, older/newer snapshot, late join, restart, stale session, end of session, window overflow, unknown types). A **randomized simulation** of 85 runs (loss, duplication, reordering, long delays, outages, lost requests and replies, slow and failed snapshots, a 256-message ring, restarts, late joins) checks the TLA+ invariants at every step: messages applied in order exactly once, and the book always equal to the publisher's as of `expected − 1`. It also checks that every recovery path actually ran. |
| Feed over sockets | Publisher → UDP → subscriber with the recovery servers on their own thread: no loss, batched with feed and reply loss, a 64-message ring (snapshot fallback), a late joiner, and multicast to two subscribers. The final books are byte-identical. `scripts/check-feed.sh` repeats this with the real tools and also checks that the publisher's book equals a plain deterministic replay's. |
| Fuzzing | `fuzz_beast`, `fuzz_decoder`, `fuzz_packet`, `fuzz_feed` (snapshot stream, subscriber datagrams, retransmit requests) with invariant checks (round trips, no out-of-bounds reads, tracker never moves backwards, replies carry exactly the logged messages) |
| Test quality | Mutation testing: planted bugs in each module are caught by the suite |

## Limits and next steps

- **Decoder gaps:** TC 29/31, Comm-B (BDS), and single-bit error correction are not decoded. The GNSS-height unit question (feet vs metres, see `decoder.hpp`) awaits the readsb differential diff on real traffic.
- **Track engine:** `nic` is not populated yet. Heading and airspeed are decoded but not carried in `TrackUpdate` v0 (an open question in `feed_spec.md`).
- **Feed:** kernel UDP only. io_uring, AF_XDP and the shared-memory ring (§9.3), hardware timestamps (§9.4), and the conflated feed (§10) are not built. IPv4 only. The subscriber's snapshot fetch blocks its thread (datagrams queue in the kernel meanwhile). The snapshot server copies the whole book on the recovery thread, which delays retransmits by that long (about 20 µs per 1,000 tracks). The retransmit server answers any requester (LAN only: no rate limit against spoofed requests). If the recovery thread stalls for long enough to fill its 65,536-entry log, the publisher waits rather than lose retransmit state. A reused session id cannot be detected by subscribers, so the tools generate a unique one per start.
- **Not yet built:** WebSocket gateway, A/B arbiter stage, IQ demodulator.
