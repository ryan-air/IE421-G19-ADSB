# ADS-B as a Market Data Feed — Project Updates

This document describes how we are extending the Fall 2025 **ADS-B Aircraft Tracking System** (see [FALL2025_README.md](FALL2025_README.md) and [PROJECT_REPORT.md](PROJECT_REPORT.md)) into a low-latency, HFT-style system. The original README, kept as `FALL2025_README.md`, still documents the existing system. This file documents **what we are changing, how, and how we will measure it**.

> **Team setup:** to build and test the new code, start with [SETUP.md](SETUP.md).

> **Status (November 2026):** built and tested in [`core/`](core/), [`protocol/`](protocol/) and [`spec/`](spec/):
> - the M0 foundations (§19);
> - the track state engine (Workstream D);
> - feed spec v0 with generated C++/JS/Python code (§9.1);
> - `.adsbrec` record/replay with the determinism check (§11.1–11.2);
> - the synthetic load generator (§11.4) and the naive baseline build (§11.5);
> - the TLA+ model of gap recovery (§12);
> - the host tuning scripts (§18).
>
> - live Beast ingest from readsb (`adsb-live`, §5.3);
> - the feed over kernel UDP (§9.1–9.2, 9.5): publisher, retransmit and snapshot servers, subscriber library and `adsb-feed-cli`, with loss injection; reconstructed books are byte-identical to the publisher's;
> - the WebSocket gateway (`gateway/`, Node): subscribes to the feed with the same recovery protocol, pushes conflated frames to browsers with per-client flow control, and serves the map from memory instead of Postgres (`ads/app.js` takes the live feed itself; it can subscribe to the full feed or the conflated feed);
> - the first end-to-end before/after comparison ([findings](bench/results/legacy-baseline/README.md)): frame → map in the browser about 1.1–1.6 s for the Fall 2025 system (83% of positions discarded) against about 70–90 ms for the new pipeline (median, 50–3,000 aircraft, laptop; tail percentiles still to be measured on a quiet host).
>
> - overload semantics for the live pipeline (§10.1, §10.3): loss only at the input boundary and counted, publisher batching while busy (1 M frames/s with publishing, up from about 300 k), tested at 0.5×, 2× and 10× saturation and through a burst (`core/scripts/check-overload.sh`, in CI).
>
> - the shared-memory broadcast ring (§9.3): same-host transport with lap detection and snapshot resync; write → read p50 125 ns, p99 209 ns at 100 k msg/s (laptop).
> - the conflated feed (§10.2): latest state per aircraft at a fixed rate on its own group, with its own recovery; after every tick its book equals the main feed's by state.
> - memory-model testing of both lock-free rings (§12): GenMC model checking with mutants, plus stress runs on x86 and ARM in CI; it found a lap-resume bug, fixed.
> - the io_uring transport (§9.3): publisher sends through io_uring with SQPOLL, no syscall per datagram; the hot thread's send call p50 208 ns against 1,459 ns for `sendto()` (laptop VM), at the cost of a polling core.
> - the AF_XDP transport (§9.3), both sides: the publisher writes our own Ethernet/IPv4/UDP frames on an XSK TX ring (in copy mode over a veth pair it matches kernel UDP, send call p50 750 ns both); the subscriber attaches a small XDP program (built without libbpf) that redirects the feed into an XSK, which takes the send → receive median from 1.71 µs to 1.13 µs even in copy mode.
> - deterministic simulation testing (§12): the real publisher, recovery side and subscribers on a simulated network and clock, a random world per seed, invariants after every step; hundreds of thousands of seeds pass, 40,000 new ones per CI run and a nightly sweep. It found four subscriber defects (two ways a subscriber could end up permanently on a dead session after a restart, and two recovery-throughput problems), fixed in C++ and JS.
>
> Ready to run on the benchmark host: `bench/lab/run.sh` (every benchmark in one command, including AF_XDP zero-copy over two cabled ports; see `bench/lab/README.md`).
>
> Not started: AF_XDP zero-copy numbers (need a NIC that supports it), hardware timestamps (§9.4, on hold), the arbiter (B), the demodulator (A), and fusion (I).

> **Core idea:** treat the 1090 MHz aircraft feed as a stand-in for an exchange market data feed, and own the whole path from **antenna to published decision**. We demodulate the radio samples ourselves, merge redundant A/B receiver lines, decode and maintain state on a lock-free pinned-core pipeline, and publish over kernel UDP, io_uring, AF_XDP, or shared memory. Every segment is measured in cycles and nanoseconds against hardware timestamps. We then show that the same infrastructure handles a real exchange feed (**Nasdaq ITCH 5.0**) at tens of millions of messages per second.

---

## Table of Contents

1. [Where We Are Starting From](#1-where-we-are-starting-from)
2. [Goals, Targets, and Non-Goals](#2-goals-targets-and-non-goals)
3. [Scope Priorities](#3-scope-priorities)
4. [Target Architecture](#4-target-architecture)
5. [Workstream A — RF Ingest and SIMD Demodulator](#5-workstream-a--rf-ingest-and-simd-demodulator)
6. [Workstream B — A/B Line Arbitration](#6-workstream-b--ab-line-arbitration)
7. [Workstream C — Mode S Decoder](#7-workstream-c--mode-s-decoder)
8. [Workstream D — Track State Engine](#8-workstream-d--track-state-engine)
9. [Workstream E — Feed Distribution and Transports](#9-workstream-e--feed-distribution-and-transports)
10. [Workstream F — Overload Semantics and Conflation](#10-workstream-f--overload-semantics-and-conflation)
11. [Workstream G — Record, Replay, and Measurement](#11-workstream-g--record-replay-and-measurement)
12. [Workstream H — Correctness and Verification](#12-workstream-h--correctness-and-verification)
13. [Workstream I — Noisy Neighbor and Camera Fusion](#13-workstream-i--noisy-neighbor-and-camera-fusion)
14. [Workstream J — Exchange Data: Nasdaq ITCH 5.0 L3 Book](#14-workstream-j--exchange-data-nasdaq-itch-50-l3-book)
15. [Stretch Goals](#15-stretch-goals)
16. [Repository Layout](#16-repository-layout)
17. [Toolchain and Dependencies](#17-toolchain-and-dependencies)
18. [Hardware and Test Environments](#18-hardware-and-test-environments)
19. [Timeline and Milestones](#19-timeline-and-milestones)
20. [Risks and Mitigations](#20-risks-and-mitigations)
21. [Open Decisions](#21-open-decisions)
22. [Professor Feedback and Progress (November 2026)](#22-professor-feedback-and-progress-november-2026)

---

## 1. Where We Are Starting From

The Fall 2025 system ([ads-b-receiver/adsb_streamer_optimized.py](ads-b-receiver/adsb_streamer_optimized.py), [ads/server.js](ads/server.js)) works like this:

```
RTL-SDR ─▶ dump1090 ──SBS-1 text (TCP 30003)──▶ Python streamer ──batch CSV COPY──▶ Supabase Postgres
                                                                                        │
                                   Browser ◀──HTTP poll every 2 s── Node/Express (2–3 s cache)
```

| Property | Fall 2025 system | Why it matters for this project |
|---|---|---|
| Demodulation | dump1090 (a separate process) | The largest latency in the system is not ours to control or measure |
| Input format | SBS-1 text, already decoded | We never see raw samples, frames, CRC, or timing |
| Language | Python and Node.js | GC, interpreter overhead, allocation on every message |
| Buffering | 100-record batches, flushed every 1 s | Latency goes into the database by design |
| Distribution | Database plus HTTP polling | One-to-many fan-out goes through a database |
| End-to-end latency | About 1–2.2 s, frame to screen (report's data-flow example) | This is our "before" number |
| Measurement | Throughput only | No per-stage latency and no tail analysis |
| Reproducibility | None | Results cannot be re-run |

### Where the time actually goes

This latency budget is why the project owns the radio path instead of starting at a decoded feed:

| Segment | Approximate latency | Who controls it today |
|---|---|---|
| RF → USB transfer → SDR sample buffer | **10–60 ms**, set by USB transfer size (librtlsdr default 256 KB ≈ 55 ms at 2.4 MS/s) | librtlsdr defaults |
| Demodulation and output batching (dump1090/readsb) | **1–100+ ms**, depending on network flush settings | another process |
| Decode → state → publish (the code we would otherwise optimize) | **1–10 µs** | us |
| Database write and poll | **1,000–2,000 ms** | Fall 2025 design |

Optimizing only the µs-scale segment would improve something that is under 0.1% of the real path. So this project takes ownership of the whole path, from the first sample of a preamble to the moment its update reaches the wire.

The existing Kalman filter ([ads/kalman-filter.js](ads/kalman-filter.js)), probability cones, collision detection, and Leaflet/Cesium frontend are reused as **subscribers** of the new feed.

---

## 2. Goals, Targets, and Non-Goals

### Goals
1. **Own the wire:** demodulate raw IQ samples in-house with a SIMD demodulator. readsb becomes a correctness oracle, outside the live path.
2. **Redundant A/B lines:** two independent radio chains, merged by a line arbiter, as exchange feed handlers do.
3. **A C++ hot path** with no allocation, no locks, and no syscalls beyond I/O, on pinned and isolated cores.
4. **A market-data-grade feed:** MoldUDP64-style sequenced multicast with retransmission and snapshots, defined overload behavior, and a conflated feed for slow consumers.
5. **Several transports, compared on the same histograms:** kernel UDP, io_uring, AF_XDP (kernel bypass), and a shared-memory broadcast ring.
6. **Hardware-timestamped measurement:** NIC timestamps and PTP for true one-way wire latency, cycle-level stage costs, and an explanation for every tail outlier class we observe.
7. **Correctness you can rely on:** TLA+-checked recovery protocol, memory-model-tested rings, deterministic simulation testing, fuzzing, and differential oracles.
8. **Generality:** the same infrastructure builds an order-by-order (L3) book from **Nasdaq ITCH 5.0** at exchange rates.

### Targets (initial, to be validated in Weeks 3–5)
| Metric | x86-64 Linux benchmark host | Raspberry Pi 5 |
|---|---|---|
| Demodulator cost | ≤ 10 cycles/sample (AVX2) | ≤ 30 cycles/sample (NEON) |
| Demodulator sensitivity | ≥ 90% of readsb's valid frames on the same IQ recording | same |
| **Sample-to-wire** (preamble sample → NIC TX timestamp) | p99 < 5 ms, versus ~50–150 ms through default readsb | p99 < 8 ms |
| Decode + state update, per frame | p99 < 300 ns | p99 < 2 µs |
| Frame ready → `sendmsg` returns (kernel UDP) | p99 < 10 µs | p99 < 40 µs |
| AF_XDP versus kernel UDP | ≥ 2× lower p99 TX latency | measured (copy mode only, see §9.3; on a veth pair copy mode equals kernel UDP) |
| Shared-memory IPC (same host) | p99 < 500 ns publish → consumer read | p99 < 2 µs |
| Pipeline throughput under replay | ≥ 2 M frames/s | ≥ 200 k frames/s |
| ITCH L3 book build (Workstream J) | ≥ 20 M msg/s per core (stretch: 50 M) | n/a |
| Frame → browser render | < 100 ms (versus about 2 s today) | same |

Reported as cycles where the work is CPU-bound and as ns or µs where it crosses a boundary.

### Non-goals
- DPDK on unsupported NICs. AF_XDP gives us kernel bypass on stock Linux, and we explain where DPDK or ef_vi would go further.
- Live trading of any kind. All exchange data is historical or public read-only. The backtester is paper only.
- Weather and visibility classification (cut from the earlier plan to protect depth).

---

## 3. Scope Priorities

The plan is deliberately prioritized so the core is finished to a high standard before breadth is added.

| Priority | Items |
|---|---|
| **Must** (core project) | A: SIMD demodulator · C: decoder · D: track state · E: multicast feed with gap recovery (kernel UDP) · F: overload semantics · G: record/replay/measurement with baselines |
| **Should** (makes it an HFT-grade systems project) | B: A/B arbitration · E: AF_XDP, hardware timestamps and PTP, shared-memory IPC · F: conflated feed · H: TLA+ and memory-model testing · I: noisy-neighbor isolation with minimal camera fusion |
| **Could** (planned, scheduled late) | J: ITCH 5.0 L3 book · E: io_uring transport · H: deterministic simulation testing |
| **Stretch** | Backtester with matching simulation · FPGA demodulator / ITCH parser · multilateration |

---

## 4. Target Architecture

```
  Antenna A ─▶ RTL-SDR A ─USB─▶ [IQ Ingest A]─SPSC─▶[Demod A]─┐
                                  core 1           core 2     │ SPSC
                                                              ▼
                                                         [Arbiter]─SPSC─▶[Decode+State]─SPSC─▶[Publish]──▶ transport
                                                           core 4           core 5             core 6       │
                                                              ▲ SPSC                                        │
  Antenna B ─▶ RTL-SDR B ─USB─▶ [IQ Ingest B]─SPSC─▶[Demod B]─┘                                             │
                                  core 3           core 3                                                   │
                                                                                                            │
  Off the live path:  readsb (oracle) · [Recorder] tap on IQ and frames · per-thread HDR histograms         │
                                                                                                            ▼
   ┌───────────── Transports (selected at startup, benchmarked against each other) ────────────────────────────┐
   │ kernel UDP multicast │ io_uring (SQPOLL) │ AF_XDP (XSK, own Eth/IP/UDP headers) │ /dev/shm broadcast ring │
   └──────────┬───────────────────────┬──────────────────────────────┬────────────────────────┬──────────────┘
              ▼                       ▼                              ▼                        ▼
       WebSocket gateway        Conflated feed (10 Hz)        Camera fusion            feed-cli / latency probe
       ─▶ browser map           ─▶ slow consumers             (noisy-neighbor test)    (HW RX timestamps)

   Gap recovery: Retransmit server (UDP unicast, last 65,536 msgs) + Snapshot server (TCP, full table "as of seq N")
```

Core numbers are for an x86 host with at least 8 isolated cores. On a 4-core Pi 5, the pipeline runs a single line (A only), with demodulation, decode, and state fused onto fewer cores. That layout is benchmarked separately.

**Design rules for the hot path**
- **One thread per stage, pinned** with `pthread_setaffinity_np` to cores isolated with `isolcpus=`, `nohz_full=`, and `rcu_nocbs=`. IRQs are steered away from those cores, and the CPU governor is set to `performance`.
- **Lock-free SPSC ring buffers** between stages. Capacity is a power of two. Head and tail sit on separate 64-byte cache lines. Each side keeps a cached copy of the other side's index. Ordering uses `acquire`/`release` only. Messages are fixed-size PODs copied by value.
- **No allocation after startup.** All memory is allocated on 2 MB huge pages where available, pre-faulted, and locked with `mlockall`. Debug builds abort if `malloc` is called on a hot thread.
- **Busy-spin consumers** with `pause` (x86) or `yield` (ARM). A backoff mode exists for fair comparison on small machines.
- **Event time, not wall time.** All state logic uses the frame's own timestamp, so replay is deterministic (§11.2).
- **Pluggable at both ends.** Sources (live SDR, IQ file, Beast TCP, frame recording, load generator, later FPGA) and transports are templated into the hot loop, so there are no virtual calls per message.

---

## 5. Workstream A — RF Ingest and SIMD Demodulator

### 5.1 IQ ingest
- Read samples directly from **librtlsdr** with `rtlsdr_read_async`, on a dedicated pinned thread. Each USB callback copies the block into a pre-allocated IQ ring and stamps it with `CLOCK_MONOTONIC_RAW` (or TSC/`CNTVCT_EL0`) and a running **sample index**.
- **USB transfer-size experiment:** sweep the transfer size from 256 KB (≈ 55 ms of samples at 2.4 MS/s, the default) down to 16 KB (≈ 3.4 ms), and the buffer count from 2 to 32. Plot sample-to-wire latency against dropped USB transfers and against CPU and interrupt load. This single tradeoff curve is the largest latency gain in the whole system.
- Sample-drop detection: gaps in the expected sample index, plus librtlsdr drop counters.

### 5.2 Demodulator
- **Stage 1 (2.0 MS/s):** one sample per 0.5 µs chip, the classic dump1090 layout. This is the simplest correct design, and gets us to M1 quickly.
- **Stage 2 (2.4 MS/s):** the readsb-style demodulator with phase tracking for better sensitivity.
- **Magnitude:** compute `|I + jQ|` with SIMD (AVX2 / NEON) using an approximation such as alpha-max-plus-beta-min. Benchmark it against the classic 64K-entry, 128 KB lookup table, which does not fit in L1. Measure cache misses for both.
- **Preamble detection:** a vectorized correlation over the 8 µs, 4-pulse preamble pattern with relative-amplitude checks. Candidates go to a scalar bit slicer that reads 56 or 112 bits.
- **CRC-24** check, as in §7.2. Output is a `RawFrame` (payload, sample index, signal level, host timestamp of the containing block), the same struct the Beast parser produces. Everything downstream is source-agnostic.
- **Timing:** we own the sample counter. A frame's radio time is `block_host_time + (sample_index − block_start) / sample_rate`. This is refined by a linear fit of sample index against host time, which absorbs USB callback jitter. The result is a **sample-to-wire** latency with a known error bound.

### 5.3 Verification and comparison
- **Differential against readsb** on the same IQ recordings (`--ifile`): per-frame match/miss/extra classification, sensitivity as a function of signal level, and false-decode rate.
- Fuzz the demodulator with noise-only and clipped IQ inputs. Invariant: it never emits a frame that fails CRC.
- **Beast TCP source retained:** readsb's Beast output on port 30005 stays as an alternative `FrameSource`. That lets the decoder (C) start before the demodulator is finished, and gives a "readsb in the path" baseline.

---

## 6. Workstream B — A/B Line Arbitration

Exchanges publish every message on two redundant feeds (A and B). Feed handlers take whichever copy arrives first and use the other only to fill gaps. ADS-B fits this naturally if we run **two independent radio chains at the same site**.

### 6.1 Setup
- Two RTL-SDRs with separate antennas, placed apart and ideally on separate USB controllers, so their losses are independent (multipath, interference, and USB drops differ per line).
- Each line runs its own ingest and demodulator threads into its own SPSC ring to the arbiter.

### 6.2 Arbitration
- ADS-B has **no sequence numbers**, so frame identity is **the payload itself** (56 or 112 bits) within a short host-time window.
- The arbiter keeps a small open-addressing table of `(payload hash → first-seen time, line)` entries with a **5 ms** window. That is far below the minimum spacing of legitimately repeated identical payloads (≥ ~400 ms for position and identity messages), so a real repeat is never discarded.
- The first arrival is forwarded and the second is dropped and counted. There is no waiting on the slower line: arbitration adds only a table lookup to the fast line's latency.

### 6.3 Measurements
- **Unique-frame contribution of each line:** frames only A received, only B received, and both.
- **Win rate and win margin:** the distribution of `t_late − t_early` for frames received on both lines. This is the latency saved by arbitration.
- **Relative clock behavior of the two dongles:** at the same site, the true arrival-time difference is about 0. Comparing the two sample counters directly measures their crystal offset and drift, which is the groundwork for multilateration (§15.4).
- Fault injection: unplug, detune, or attenuate one line, and show that the output feed has no gap.

---

## 7. Workstream C — Mode S Decoder

### 7.1 Scope
| Downlink Format | Content | CRC handling |
|---|---|---|
| **DF17** | ADS-B Extended Squitter (main target) | Parity must be 0. ICAO address is in the clear. |
| **DF18** | TIS-B / ADS-R / non-transponder | Parity must be 0 |
| **DF11** | All-call reply | Parity XOR interrogator ID; accept IID 0 only |
| DF4/5/20/21 | Surveillance altitude/identity | Recover ICAO from the CRC residual; accept **only if the ICAO is already a live track** |

DF17 type codes decoded:
| TC | Message | Fields |
|---|---|---|
| 1–4 | Identification | Callsign (6-bit charset, 8 chars), emitter category |
| 5–8 | Surface position | Surface CPR (local decode against receiver position), ground speed, track |
| 9–18 | Airborne position (baro altitude) | CPR lat/lon, 12-bit altitude (Q-bit 25 ft or Gillham 100 ft) |
| 19 | Airborne velocity | Subtypes 1/2 (ground speed) and 3/4 (airspeed and heading), vertical rate |
| 20–22 | Airborne position (GNSS height) | As above, with geometric altitude |
| 28 / 29 / 31 | Status, target state, operational status | Squawk/emergency, selected altitude, NIC/NAC/SIL |

### 7.2 Implementation details
- **CRC-24** (generator `0xFFF409`): a 256-entry, 1 KB table, so it stays in L1. Slice-by-4 and a carry-less-multiply (PCLMULQDQ / PMULL) variant are benchmarked. Single-bit error correction is off by default, and its effect on false decodes is measured.
- **Bit extraction:** load the 112-bit message into two `uint64_t` registers and extract fields with constant shifts and masks.
- **CPR decoding:**
  - **Global decode** needs an even/odd pair from the same aircraft within 10 s (event time). The NL lookup is a precomputed 59-entry threshold table, so no trigonometry runs on the hot path.
  - **Local decode** against the last known position, or the receiver position.
  - **Reasonableness check:** reject a position if the implied speed is above Mach 1 or the jump exceeds 180 NM. Rejections are counted.
- **Output:** a fixed-size `DecodedMsg` POD. No strings anywhere.

### 7.3 Verification
1. Known vectors from *The 1090 MHz Riddle* (J. Sun) and the **pyModeS** test suite.
2. **Differential oracle:** the same recording is decoded by readsb (fed through its Beast input on port 30004) and by our decoder. Fields are diffed per frame. **pyModeS** is a second oracle.
3. **libFuzzer** with ASan and UBSan, seeded with real frames.
4. Property tests: CPR and altitude encode → decode round trips across the full input range.

---

## 8. Workstream D — Track State Engine

### 8.1 Data structure
- An **open-addressing hash table** keyed by 24-bit ICAO. Capacity is a power of two (default 8,192). It uses linear probing, a Fibonacci hash, and backward-shift deletion (no tombstones).
- **A cache-conscious split:** an index array of `icao` and `slot` (8 B per entry, 8 per cache line), a **64-byte cache-line-aligned hot record** (position, altitude, velocity, timestamps, update bitmask), and a cold record (callsign, squawk, quality, CPR buffers, counters).
- **Baselines:** `std::unordered_map<uint32_t, Track>` and `absl::flat_hash_map`.

### 8.2 Semantics
| Concern | Rule |
|---|---|
| **Duplicates** | Removed across lines by the arbiter (§6). Within a single line, a per-track ring of `(payload hash, time)` catches demodulator double-detections. |
| **Out-of-order frames** | Each field group keeps its own event timestamp. Older updates are dropped and counted. |
| **Stale-track expiry** | A **timing wheel** (1 s buckets, 64 s horizon) driven by event time. A track is evicted after 60 s of silence and a `TrackDelete` is published. |
| **Change detection** | A `TrackUpdate` is published only when a field actually changes. The bitmask tells subscribers which fields changed. |

### 8.3 Benchmarks
- Google Benchmark: lookup hit, lookup miss, insert, and update at 100 / 1k / 10k tracks, with uniform and Zipf-skewed key distributions. Reported as ns/op, cycles/op, and cache misses/op.
- End-to-end state-stage histograms under replay at 1×, 100×, and max speed, against both baselines.

---

## 9. Workstream E — Feed Distribution and Transports

### 9.1 Framing: modeled on Nasdaq MoldUDP64
**Packet header (20 bytes):** `session` char[10] (changes on every publisher restart), `seq` u64 (sequence number of the first message), `count` u16 (`0` = heartbeat, `0xFFFF` = end of session). It is followed by `count` message blocks, each prefixed with a `u16` length.

Byte order: **little-endian for our own ADS-B messages**, a deliberate deviation recorded in the spec. **ITCH payloads (§14) are carried unchanged in their native big-endian form**, because MoldUDP64 is ITCH's real transport, so the framing layer is shared exactly.

**`TrackUpdate` (64 bytes, fixed). Draft layout:**
| Offset | Field | Type | Unit |
|---|---|---|---|
| 0 | `msg_type` | u8 | `'U'` |
| 1 | `version` | u8 | 1 |
| 2 | `update_mask` | u16 | which fields changed |
| 4 | `icao` | u32 | lower 24 bits |
| 8 | `event_ts_ns` | u64 | radio time of the frame (§5.2) |
| 16 | `sample_index` | u64 | per-line sample counter, with the line ID in the top 4 bits |
| 24 | `lat_e7` / `lon_e7` | i32 ×2 | 1e-7 deg |
| 32 | `alt_ft` | i32 | ft |
| 36 | `gs_dkt` | u16 | 0.1 kt |
| 38 | `track_cdeg` | u16 | 0.01 deg |
| 40 | `vrate_fpm` | i16 | ft/min |
| 42 | `squawk` | u16 | BCD |
| 44 | `callsign` | char[8] | space-padded |
| 52 | `category`, `nic`, `flags` | u8 ×3 | `flags` includes baro/GNSS altitude and winning line |
| 55 | reserved | u8 | |
| 56 | `publish_ts_ns` | u64 | software timestamp taken just before transmit |

Other message types: `TrackDelete` (16 B), `FusionMatch` / `UnidentifiedObject` (fusion), and ITCH messages (unchanged). The spec lives in `protocol/feed_spec.md`. **C++, JavaScript, and Python struct definitions are generated from one schema file**, and CI checks a golden packet across all three.

### 9.2 Publishing policy
- A packet holds 20 + 22 × (2 + 64) = **1,472 bytes**, which fits a 1500-byte MTU with no fragmentation.
- Flush policy is part of the benchmark: `immediate` versus `batch` (flush when full or after N µs). We plot the latency/throughput tradeoff.
- `IP_MULTICAST_TTL=1`, **wired Ethernet only**. Wi-Fi multicast is rate-limited and lossy.

### 9.3 Transports (selected at startup, compared on identical histograms)
| Transport | Implementation notes |
|---|---|
| **Kernel UDP** | `sendmsg`/`recvmmsg` with `SO_BUSY_POLL`. This is the baseline. |
| **io_uring** | `IORING_SETUP_SQPOLL`, so a kernel thread polls submissions and no syscall is made per packet. Registered socket; a fixed buffer pool (registered buffers only pay off for zero-copy sends, which cost more than a copy for datagrams this small). Built: `--transport uring`, see `core/README.md`. |
| **AF_XDP** | Publisher: an XSK socket in TX mode, building Ethernet, IPv4, and UDP headers ourselves (multicast MAC `01:00:5e` plus the low 23 bits of the group address). Subscriber: a small XDP/eBPF program redirects our UDP port into the XSK map and passes everything else to the kernel. **Zero-copy needs driver support** (Intel `igc` / `i40e` / `ice`, Mellanox `mlx5`). The Pi 5's NIC runs in copy mode only, and we report both. Built, both sides: publisher `--transport xdp`, raw `<linux/if_xdp.h>`, copy/zero-copy/auto, copy mode over veth equal to kernel UDP; subscriber `--rx xdp`, the redirect program as eBPF instructions loaded with `bpf()` and attached through a BPF link, send → receive p50 1.13 µs against 1.71 µs through the kernel socket (veth, copy mode). See `core/README.md`. |
| **Shared-memory broadcast ring** | For subscribers on the same host: a single-producer, multi-consumer log on `/dev/shm` backed by huge pages (Aeron / LMAX Disruptor style). Each consumer keeps its own cursor. A consumer that falls a full lap behind detects it from the sequence number and recovers from a snapshot. |

### 9.4 Hardware timestamps and clock sync
- `SO_TIMESTAMPING` with `SOF_TIMESTAMPING_TX_HARDWARE` and `RX_HARDWARE` on publisher and subscriber NICs. Check support with `ethtool -T`.
- **linuxptp:** `ptp4l` syncs the NIC clocks between hosts, and `phc2sys` syncs each NIC clock to its system clock. We report the PTP offset alongside every cross-host result.
- That splits each packet's journey into measured segments: **software → NIC TX** (stack latency), **TX → RX** (wire plus switch, or direct cable), and **NIC RX → application** (receive path).

### 9.5 Gap detection and recovery
1. On `seq > expected_seq`, the subscriber buffers what arrived and sends a unicast **retransmit request** to the retransmit server, which holds a ring of the last 65,536 messages (about 4 MB).
2. If the gap is older than that ring, the subscriber falls back to the **snapshot server** (TCP): full table "as of seq N", then the buffered messages with `seq > N`.
3. Heartbeats every 1 s, so a subscriber detects a gap even when traffic stops.
4. Fault injection (`--drop-rate`, `--drop-burst`, `tc netem`). The test asserts that the reconstructed table is **byte-identical** to the publisher's.

### 9.6 Subscribers
| Subscriber | Language | Purpose |
|---|---|---|
| `feed-cli` | C++ | Sequence verification and latency probe (software and hardware RX timestamps) |
| **WebSocket gateway** | Node (`dgram` multicast) | Pushes binary frames to the existing Leaflet/Cesium UI, **replacing the 2 s database polling**. Subscribes to the conflated feed. |
| Camera fusion | Python | §13 |
| Archiver (optional) | Python | Keeps the existing Postgres history, statistics, and trails working |

---

## 10. Workstream F — Overload Semantics and Conflation

A feed handler is judged by what it does when it cannot keep up, not only by how fast it is when it can.

### 10.1 Overload policy at each boundary
| Boundary | Policy | Reason |
|---|---|---|
| SDR → IQ ring | **Drop the sample block** and count it | The radio cannot be paused. Loss is recorded at the single place it can occur. |
| IQ ring → demodulator → arbiter → decode/state → publish | **Producer spins** (lossless inside the pipeline) | Internal stages stay lossless, so pressure shows up at the SDR boundary as counted drops. |
| Publisher → network | **Never block**. A socket-buffer or XSK ring overflow is counted. | Subscribers recover through gap recovery. |
| Shared-memory ring → consumer | **Producer never waits**. A lapped consumer resyncs from a snapshot. | A slow consumer must never slow the producer. |

### 10.2 Conflated feed
- A separate multicast group carrying **only the latest state per aircraft** at a configurable rate (default 10 Hz). It is built from a per-ICAO dirty bitmap, so the cost is proportional to the number of changed aircraft, not total aircraft.
- The browser gateway and camera fusion subscribe here, as real vendors do with slow consumers.

### 10.3 Measurements
- Run the load generator at 1×, 2×, and 10× the measured saturation rate. Report where loss occurs, how much, how quickly the system recovers, and how latency degrades. Healthy degradation means a bounded queue delay and loss counted only at the SDR boundary, with no crash and no unbounded memory.

---

## 11. Workstream G — Record, Replay, and Measurement

### 11.1 Recording formats
- **`.iqrec`:** raw 8-bit IQ samples with periodic `(sample_index, host_time)` anchors and a header (line ID, sample rate, gain, center frequency, receiver position). It is the input for the demodulator, the readsb diff, and the FPGA work.
- **`.adsbrec`:** frames (`rx_ns`, `sample_index`, line, type, signal, payload) with a header and a footer CRC. It is the input for the decoder and state benchmarks.
- Recorders are taps on their own SPSC rings, so disk I/O never blocks the hot path. A full ring drops records and counts them.
- **Datasets:** a quiet hour, a peak hour, a 24-hour frame run, and A/B IQ captures (a few minutes at a time, about 4.8 MB/s per line at 2.4 MS/s).

### 11.2 Deterministic replay
- Speeds: `1×`, `N×`, `max`.
- **Determinism contract:** for a given recording and build, the published message stream, excluding wall-clock timestamp fields, is **byte-identical** across runs and speeds. CI compares a SHA-256 of the stream against a golden hash.

### 11.3 Measurement harness
- **Timestamp points:** first preamble sample (radio time) → frame demodulated → arbitrated → decoded → state updated → transmitted (software) → **NIC TX (hardware)** → **subscriber NIC RX (hardware)** → subscriber application.
- **Histograms:** per-thread, fixed-memory, log-linear (HdrHistogram-style, 1 ns to 10 s), merged after the run. Reported: p50, p90, p99, p99.9, p99.99, max, and throughput. CPU-bound stages are also reported in **cycles** using `rdtsc` / `CNTVCT_EL0` and `perf stat` counters.
- **Coordinated omission:** latency is measured from the **intended** send time during fixed-rate replay.
- **Methodology:** warmup, ≥ 5 runs per configuration, pinned frequency, isolated cores, and host configuration recorded in every result file (`bench/results/*.json`).

### 11.4 Synthetic load generator
- Multiplies traffic from a recording by remapping ICAOs (recomputing CRC) and offsetting positions: **10,000+ aircraft**, up to millions of frames per second.
- Injects pathological cases: duplicate bursts, reordered frames, corrupt CRCs, and split CPR pairs.
- Also generates **synthetic IQ**: frames modulated into noise at a chosen signal-to-noise ratio, for demodulator sensitivity curves with known ground truth.

### 11.5 Baselines: before versus after
| Comparison | "Before" | "After" |
|---|---|---|
| **System level** | Fall 2025: dump1090 → SBS-1 → Python → Postgres → 2 s poll | IQ → own demodulator → A/B → C++ → multicast → WebSocket |
| **Radio path** | readsb in the path, default buffers | Own demodulator, tuned USB transfer size |
| **Implementation level** (same code, chosen by a build flag) | Mutex and condition-variable queues, `unordered_map`, `std::string`, heap allocation per message, unpinned threads | SPSC rings, open addressing, POD messages, no allocation, pinned and isolated cores |
| **Transport** | Kernel UDP | io_uring, AF_XDP, shared memory |
| **Ablation** | Each optimization turned off one at a time | Shows which change bought the improvement |

### 11.6 Tail analysis
Every p99.9 and max value is explained, not just reported. Tools: `perf record` / `perf stat`, `perf sched`, `ftrace`, `bpftrace` (scheduler and IRQ events lined up against outlier timestamps), and `/proc/interrupts`. Candidate causes, tested one at a time: page faults, IRQs, frequency transitions, SMT siblings, timer ticks, USB callback jitter, hash table probe-length spikes, and last-level-cache contention (§13).

---

## 12. Workstream H — Correctness and Verification

| Technique | Applied to | What it proves |
|---|---|---|
| **TLA+ model plus TLC model checking** | Sequenced feed, retransmit, snapshot, and session restart, under loss, duplication, and reordering | Safety: every message is applied exactly once, in sequence order. Liveness: a subscriber that keeps receiving packets eventually matches the publisher's state. Checked within bounded model sizes. |
| **Memory-model testing** | SPSC ring and shared-memory broadcast ring | ThreadSanitizer, multi-hour stress runs on **ARM** (the Pi 5, where weak memory ordering exposes bugs x86 hides) and on x86, plus **GenMC** model checking of the ring core written with C11 atomics |
| **Deterministic simulation testing** | Publisher, retransmit/snapshot servers, and subscribers in one process, with simulated network and clock | Millions of seeded fault schedules in nightly CI. Any failure replays exactly from its seed. Built: `adsb-dst` (`core/include/adsb/feed/dst.hpp`), CI jobs `dst` and `dst-nightly`. |
| **Fuzzing** (libFuzzer + ASan/UBSan) | Beast parser, Mode S decoder, demodulator, ITCH parser | No crashes, no out-of-bounds reads, no invalid frames emitted |
| **Differential oracles** | Demodulator against readsb (IQ), decoder against readsb and pyModeS, ITCH book against a reference implementation (§14) | Agreement within documented tolerances |
| **Determinism hash** | Full pipeline replay | Byte-identical output across runs |

---

## 13. Workstream I — Noisy Neighbor and Camera Fusion

Camera fusion is kept, but its main purpose is now a systems experiment. **Colocated workloads compete with the pipeline for cores, cache, and memory bandwidth**, which is a real production concern for trading servers. The fusion process is a realistic noisy neighbor: CPU-heavy, cache-thrashing, and bursty.

### 13.1 Interference experiment
Run fusion inference, plus `stress-ng` memory workers as a controlled second neighbor, **on the same host** as the pipeline, and measure the pipeline's histograms in each configuration:

| Configuration | Expected effect |
|---|---|
| Pipeline alone | Baseline |
| + neighbor, no isolation | Tail latency grows from preemption and cache contention |
| + core isolation (`isolcpus`, `nohz_full`) | Removes preemption. Last-level-cache contention remains. |
| + **Cache Allocation Technology** (`resctrl` L3 partition) | Removes most last-level-cache contention |
| + **Memory Bandwidth Allocation** | Limits the neighbor's DRAM bandwidth |
| + IRQ affinity | Removes interrupt noise on the pipeline cores |

This requires Intel RDT or AMD PQoS (check `/sys/fs/resctrl` and the `cat_l3` / `rdt_a` CPU flags). If the benchmark host lacks it, we report core isolation results and document the gap.

### 13.2 Minimal fusion pipeline
1. **Camera:** a Pi HQ Camera or a USB camera with telephoto lens, pointed at the **CMI approach path**, where aircraft are large enough in the image.
2. **Timing:** sensor timestamps from libcamera / V4L2. chrony on every host, with offset logged.
3. **Extrapolation:** a constant-velocity Kalman filter in ENU coordinates (a port of [kalman-filter.js](ads/kalman-filter.js)) predicts each track's position at the exact frame time, consuming the **conflated feed**.
4. **Projection:** geodetic → ECEF → ENU → camera frame → pixel, with the covariance projected to a pixel uncertainty ellipse. Intrinsics come from a checkerboard. Extrinsics come from `solvePnP` using matched ADS-B aircraft as calibration targets.
5. **ROI detection:** run only inside each track's 3σ ellipse, plus a **1 Hz full-frame scan** to find aircraft that are not broadcasting.
6. **Matching:** Hungarian assignment on Mahalanobis distance with χ² gating. A detection unmatched for N frames is published as `UnidentifiedObject` and highlighted in the GUI.

### 13.3 Measurements
Inference latency with ROI versus full frame, projection error, match rate, and clock offset. The interference matrix from §13.1 is the main result.

---

## 14. Workstream J — Exchange Data: Nasdaq ITCH 5.0 L3 Book

This proves the infrastructure is market-data infrastructure, not ADS-B-only code, using a real exchange's binary protocol at real exchange rates.

### 14.1 Data
- **Nasdaq TotalView-ITCH 5.0** historical sample files, published by Nasdaq. Each is a full trading day of order-by-order messages, several GB compressed and hundreds of millions of messages.
- Message types: system event, stock directory, **Add Order (`A`, `F`)**, **Order Executed (`E`, `C`)**, **Cancel (`X`)**, **Delete (`D`)**, **Replace (`U`)**, Trade (`P`), and others.

### 14.2 Pipeline reuse
- File source → **MoldUDP64 replay over our own multicast and transports** (ITCH's native transport) → subscriber-side ITCH parser → **L3 book builder** → top-of-book updates published as a feed.
- The same SPSC rings, histograms, replay modes, fault injection, gap recovery, and load generator are used unchanged.

### 14.3 Book design
- **Order store:** order reference numbers are dense and increasing over a day, so orders live in a **direct-indexed array** or flat table keyed by reference number, avoiding a general hash map.
- **Price levels:** for each symbol, arrays indexed by price tick around a moving center, with a sorted fallback for far-from-market prices. Best bid and ask are tracked incrementally.
- **Symbols:** locate codes map directly to per-symbol book slots, with no string handling on the hot path.

### 14.4 Validation and targets
- Invariants: no negative quantities, every execute/cancel/delete refers to a live order, books never cross outside auction periods, and the book matches a reference implementation's top-of-book at sampled timestamps.
- **Throughput target:** ≥ 20 M msg/s per core on a full-day file (stretch: 50 M), with per-message-type cycle costs and end-to-end histograms through the multicast stack.

---

## 15. Stretch Goals

Each stretch goal is planned to build on the core rather than start from scratch.

### 15.1 Backtester with matching simulation
- An event-driven strategy API (`on_book_update`, `on_trade`, `on_timer`), identical in replay and live paper mode.
- A **price-time priority matching simulation** using the **queue position** estimated from the L3 book, with configurable feed latency and order latency distributions.
- Main output: **PnL against latency** for a simple strategy, the curve that justifies the rest of the project. **Paper only.**

### 15.2 FPGA
- **Demodulator:** preamble detector, PPM bit slicer, and streaming CRC-24 in SystemVerilog, emitting the same `RawFrame` format. Verified in **Verilator** against `.iqrec` files, diffed against our software demodulator and readsb.
- **ITCH parser and top-of-book:** the classic FPGA market data design, verified against our software book.
- Deployed to a board if one is available. Measured in clock cycles per message against the software paths.

### 15.3 Hardware SDR alternative
- If a higher-end SDR with an FPGA (for example a USRP-class or LimeSDR-class device) is available, compare it against the RTL-SDR on sample-to-wire latency and sensitivity.

### 15.4 Multilateration
- **Needs:** ≥ 3 stations (≥ 4 for 3D without barometric altitude), several km apart, with overlapping coverage.
- **Clock sync** builds on the dongle drift characterization from A/B (§6.3). **Reference aircraft** that broadcast their own ADS-B positions let us solve continuously for each station's clock offset and drift.
- **Solver:** TDOA equations by Gauss-Newton or Levenberg-Marquardt. Validated against ADS-B-equipped aircraft's own positions. The 2.4 MS/s sample period (≈ 417 ns, about 125 m) is the raw resolution; sub-sample interpolation of the preamble improves on it.

---

## 16. Repository Layout

New directories are added next to the existing ones. The Fall 2025 code is kept as the system-level baseline.

```
group_19_project/
├── README.md                 # Fall 2025 system (to be updated)
├── README_UPDATES.md         # This document
├── PROJECT_REPORT.md         # Fall 2025 report (to be updated)
├── core/                     # NEW — C++20 low-latency pipeline
│   ├── CMakeLists.txt
│   ├── include/adsb/         #   spsc_ring, shm_ring, clock, histogram, iq, demod, arbiter, beast, modes, cpr, track_table, feed
│   ├── src/                  #   ingest/, demod/, arbiter/, decode/, state/, publish/, transports/{udp,uring,xdp,shm}/, recovery/, conflate/
│   ├── tools/                #   iq-recorder, frame-recorder, replayer, loadgen, feed-cli
│   ├── tests/  fuzz/  bench/
│   └── scripts/              #   tune-host.sh, ptp-setup.sh, resctrl-setup.sh
├── itch/                     # NEW — ITCH 5.0 parser, L3 book, MoldUDP64 replayer
├── backtest/                 # NEW (stretch) — strategy API, matching simulation
├── fpga/                     # NEW (stretch) — SystemVerilog + Verilator testbenches
├── spec/                     # NEW — TLA+ models of the feed protocol
├── protocol/                 # NEW — feed_spec.md + schema → C++ / JS / Python generators
├── gateway/                  # NEW — Node multicast → WebSocket bridge
├── fusion/                   # NEW — Python camera fusion
├── bench/results/            # NEW — benchmark JSON + plots
├── data/recordings/          # NEW — small samples (large files kept outside git)
├── ads/                      # Existing web app — modified to consume the gateway
├── ads-b-receiver/           # Existing Python streamer — "before" baseline
└── data-generator/           # Existing mock generator
```

---

## 17. Toolchain and Dependencies

| Area | Choice |
|---|---|
| Language | C++20 (GCC ≥ 12 / Clang ≥ 15), `-O3 -march=native`, LTO, **profile-guided optimization** for benchmark builds |
| Build | CMake + Ninja. Presets: `debug-asan`, `tsan`, `release`, `bench`, `bench-pgo`, `naive-baseline` |
| SDR | librtlsdr; readsb (oracle and baseline) |
| Kernel interfaces | liburing; AF_XDP and eBPF through raw syscalls (`<linux/if_xdp.h>`, `bpf()`), no libxdp / libbpf; `SO_TIMESTAMPING`, `resctrl` |
| Time sync | linuxptp (`ptp4l`, `phc2sys`), chrony |
| Testing | GoogleTest, Google Benchmark, libFuzzer, ASan, UBSan, TSan |
| Formal methods | TLA+ with TLC; GenMC |
| Reference libraries | Abseil `flat_hash_map` (benchmark baseline only), pyModeS |
| Fusion | Python 3.11, OpenCV, NumPy, ONNX Runtime |
| Profiling | `perf`, `ftrace`, `bpftrace` |
| CI | GitHub Actions ([`.github/workflows/core.yml`](.github/workflows/core.yml)): GCC + Clang × debug/ASan/TSan/release, libFuzzer smoke run, benchmark build; later: oracle diffs, determinism hash, TLC model check, nightly simulation runs |

**Platform note:** macOS can be used to edit and to run unit tests, but **all latency numbers come from Linux**. macOS has no hard thread affinity, no `isolcpus`, no AF_XDP, and no `perf`.

---

## 18. Hardware and Test Environments

| Environment | Hardware | Used for |
|---|---|---|
| **Receiver site** | **2× RTL-SDR (v4 preferred) + 2× 1090 MHz antennas** (A/B), on separate USB controllers | Live A/B capture |
| **Benchmark host** | x86-64 Linux, ≥ 8 physical cores, **Intel RDT or AMD PQoS**, SMT off for benchmark runs | Headline numbers, noisy-neighbor and transport experiments |
| **NICs** | **2× NIC with hardware timestamping and AF_XDP zero-copy** (for example Intel i210/i225/i226, X710, E810), direct cable between hosts | Wire latency, PTP, kernel bypass |
| **Second host** | Any Linux box with a matching NIC | Cross-host subscriber |
| **Raspberry Pi 5** | 4 cores, RTL-SDR | ARM results, weak-memory-ordering testing, field demo |
| **Camera node** | Pi HQ Camera or USB camera + telephoto | Fusion |
| **Stretch** | FPGA board; extra receiver kits for MLAT | §15 |

Host tuning is scripted in `core/scripts/`: core isolation, IRQ affinity, governor, huge pages, PTP, and `resctrl` partitions. The applied configuration is captured in every result file.

---

## 19. Timeline and Milestones

The plan assumes a 14-week semester. The Beast TCP source lets the decoder and state work start before the demodulator is finished.

| Week | Milestone | Exit criteria |
|---|---|---|
| 1–2 | **M0 — Foundations** | CMake and CI working. SPSC ring, clock, and histogram done with tests. Hardware capability checks (`ethtool -T`, AF_XDP zero-copy, RDT). First IQ and frame recordings. Feed spec v1. |
| 2–5 | **M1 — Demodulator + decoder** (in parallel) | Demodulator at 2.0 MS/s reaching ≥ 90% of readsb on IQ recordings. USB transfer-size sweep done. Decoder ≥ 99.9% agreement with the oracle. Fuzzers in CI. |
| 4–6 | **M2 — Track state + A/B arbitration** | Open-addressing table with expiry. Microbenchmarks against both baselines. Two-line arbitration live, with per-line contribution and win-margin results. |
| 5–7 | **M3 — Feed (kernel UDP)** | Publisher, retransmit and snapshot servers, `feed-cli`, gateway. The web UI runs off the feed. **TLA+ model checks.** Gap recovery passes under random and burst loss. |
| 7–9 | **M4 — Measurement + overload** | Determinism hash in CI. Load generator at 10k aircraft. Naive baseline. Overload tests at 2× and 10×. Conflated feed. **First full benchmark report.** |
| 8–11 | **M5 — Transports and isolation** | AF_XDP, shared-memory ring, and hardware timestamps with PTP across two hosts. Noisy-neighbor matrix with CAT/MBA. Minimal fusion live (from week 6 in parallel). |
| 10–12 | **M6 — ITCH L3 book** | Full-day ITCH file replayed through our MoldUDP64 stack. Book invariants pass. Throughput and per-message costs published. io_uring transport if time allows. |
| 12–13 | **M7 — Tuning and tail analysis** | PGO and ablation table done. Every p99.9 outlier class explained. Targets in §2 met or the gap explained. Deterministic simulation testing in nightly CI. |
| 13–14 | **M8 — Final** | Final report, demo video, reproducible benchmark instructions. Stretch goals only if M0–M7 are complete. |

**Suggested ownership** for a team of four: (1) RF ingest, demodulator, and A/B arbitration; (2) decoder, track state, and ITCH book; (3) feed, transports, PTP, and TLA+; (4) measurement, overload, noisy neighbor, and fusion.

---

## 20. Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| The in-house demodulator is less sensitive than readsb | Fewer frames | Start at 2.0 MS/s for correctness, then move to 2.4 MS/s. readsb's Beast source stays available. Report sensitivity honestly. |
| Small USB transfers cause sample drops | Latency gain costs data | That tradeoff is the experiment. We pick the knee of the curve and report both axes. |
| Benchmark NIC lacks hardware timestamps or AF_XDP zero-copy | Weaker transport results | Check in week 1. Buy a cheap Intel i210/i226 card if needed. Copy-mode AF_XDP is still reported. |
| Benchmark CPU lacks RDT | No CAT/MBA rows | Check in week 1. Report the isolation-only rows otherwise. |
| Live ADS-B traffic is too low to stress the pipeline | Weak throughput story | Synthetic load generator, plus the ITCH workstream for real exchange-rate data. |
| Scope is large | Breadth over depth | Strict Must/Should/Could tiers (§3). Must items finish to report quality before Should items start. |
| Wi-Fi multicast is unreliable | Spurious gaps | Wired Ethernet only. |
| Formal methods take longer than expected | Late M3 | Keep the TLA+ model to the protocol core (sequence, retransmit, snapshot). Session restart is optional. |

---

## 21. Open Decisions

These are team and hardware questions to settle in Week 1:

1. **Workstream owners** (see the suggestion in §19).
2. **Benchmark host:** which x86 Linux machine with root, ≥ 8 cores, and RDT?
3. **NICs:** do our hosts have hardware timestamping and AF_XDP zero-copy, or do we buy Intel i210/i226 cards?
4. **Second RTL-SDR and antenna** for the B line: purchase or borrow?
5. **Camera hardware and a site** with a view of the CMI approach path.
6. **FPGA board availability** through the course.
7. **ITCH data:** confirm access to and storage for full-day Nasdaq sample files.
8. **Large recordings:** Git LFS or external storage?

---

## 22. Professor Feedback and Progress (November 2026)

### 22.1 What Prof. Lariviere asked for

From his message of 2026-11-19:

1. He especially likes the **ADS-B signal generator**. It could be very useful for testing more advanced software-defined radios.
2. **Check whether MATLAB has something similar.**
3. The generator should simulate **many aircraft at different altitudes and locations**, with **atmospheric effects, multipath, and especially Doppler shift**.
4. A study he is curious about: **the frequency stability of the 1090 MHz transmitters on different aircraft types.**

This raises the synthetic IQ generator (already a Must item in Workstream A, §5) from a test fixture for our demodulator to a deliverable of its own, and adds a measurement study to the plan.

### 22.2 Progress so far

On `main`, with CI passing:
- the core pipeline (decoder, track state, record/replay, synthetic traffic, live ingest from readsb);
- the sequenced feed over kernel UDP with retransmit and snapshot recovery;
- the WebSocket gateway serving the Fall 2025 map without a database;
- overload semantics;
- the shared-memory ring;
- the conflated feed;
- the TLA+ and GenMC models;
- the before/after benchmark: frame → map in the browser about 1.1–1.6 s for the Fall 2025 system against about 70–90 ms for the new pipeline (median, laptop);
- io_uring and AF_XDP transports (AF_XDP on both the publishing and the receiving side, copy mode);
- deterministic simulation testing of the feed. Millions of seeded fault schedules found and fixed four recovery bugs in the subscriber.

Not started: the demodulator and the IQ generator (A), A/B arbitration (B), hardware timestamps (§9.4), ITCH (J), and camera fusion (I).

### 22.3 What MATLAB offers

Nothing like the generator described above.

| MATLAB resource | What it does | What it lacks |
|---|---|---|
| Communications Toolbox example *Airplane Tracking Using ADS-B Signals* (and its Simulink version) | Receives and decodes ADS-B from captured IQ or a live SDR (12 MS/s processing) | Receive side only; no traffic or channel simulation |
| SoC Blockset example *HW/SW Co-Design Implementation of ADS-B Transmitter and Receiver* (AD9361/AD9364) | Generates one ADS-B waveform on an FPGA-based radio | One transmitter, no aircraft geometry, Doppler, or multipath; tied to that hardware |
| General channel blocks (frequency offset, fading channels) | Building blocks that could be assembled by hand | No ADS-B scenario model |

MATLAB is still useful to us as an **independent oracle**: its receiver example should decode the IQ files our generator writes (§22.4).

### 22.4 IQ scenario generator (Workstream A)

**Owner:** Role 1 (it extends the existing Must item "Synthetic IQ generator with ground truth"). Who builds which part is to be agreed in the team.

**Scenario.** Reuse `adsb-synth`'s traffic and ground truth. Each aircraft has a position, altitude, velocity, transmit power class, and its own transmitter frequency model. The receiver has a location, an antenna pattern, and a model of its own impairments.

**Signal chain per transmission:**

| Effect | Model |
|---|---|
| Waveform | Mode S pulse-position modulation: 8 µs preamble, 56 or 112 bits of 1 µs, from the frames `adsb-synth` already encodes |
| Geometry | Free-space path loss, propagation delay, elevation-dependent antenna gain |
| **Doppler** | Shift from the radial velocity: up to about ±0.9 kHz at 250 m/s and ±1.1 kHz at 300 m/s, changing as an aircraft passes the receiver |
| Transmitter frequency | Per-aircraft offset within the ±1 MHz tolerance, plus drift and jitter models (this feeds §22.5) |
| Multipath | Two-ray ground reflection (delay and strength from the geometry) plus optional fixed reflectors such as buildings |
| Atmosphere | Refraction (4/3-Earth radius) for the radio horizon, so aircraft fade out realistically; gaseous absorption is tiny at 1 GHz (under 0.01 dB/km) and rain is negligible at L band. Optional ducting for anomalous range |
| Collisions | Overlapping replies from different aircraft add together, as in busy airspace (the main cause of lost messages) |
| Receiver | Thermal noise, its own frequency error (ppm), 8-bit quantization and clipping, DC offset, IQ imbalance |

**Outputs:**
- IQ files in the common formats: `cu8` for RTL-SDR and readsb `--ifile`, `cs8`, `cs16` and `cf32`, at configurable rates (2.0, 2.4 and 12 MS/s and more);
- **SigMF** metadata with per-message truth annotations: sample index, aircraft, SNR, frequency offset, and whether it collided.

**Validation:**
- readsb and the MATLAB receiver example both decode the files;
- decode rates follow the truth (SNR, collisions);
- our demodulator (§5) is scored against the same truth.

**Safety and legality:** generated files may be played into an SDR receiver only **by cable through attenuators, or inside a shielded enclosure**. Transmitting on 1090 MHz over the air is illegal and could interfere with air traffic control.

### 22.5 Transponder frequency-stability study

**Question:** how far is each aircraft's transmitter from 1090 MHz, how much does it drift within a flight and between days, and does that depend on the aircraft or transponder type? The standard allows ±1 MHz (ITU-R M.2413). The only field measurement we found is a 1972 MIT Lincoln Laboratory study of about 550 transponders.

**Method:**
1. **Estimate the carrier frequency of every decoded message** from the IQ samples: phase slope across the pulses, refined with the known bit pattern once the message is decoded.
2. **Subtract the Doppler**, computed from the aircraft's own decoded position and velocity. It is at most about 1 kHz, small next to the offsets but larger than the drift we want to see.
3. **Subtract the receiver's own frequency error.** Use a GPS-disciplined SDR, or calibrate an RTL-SDR against a reference signal or the average over many aircraft.
4. **Group the results:**
   - by aircraft (ICAO address);
   - by type, from a public aircraft database and the ADS-B emitter category;
   - over time, within a flight and across days (Allan deviation).

**Needs:**
- a receiver sampling at 2.4 MS/s or more, so that offsets up to ±1 MHz stay in band;
- long IQ recordings;
- ideally a GPS-disciplined reference.

**Before the hardware:** the generator (§22.4) tests the whole method on synthetic aircraft whose true offsets and drift we know.

### 22.6 Hardware and data we still need

| Need | For |
|---|---|
| Benchmark Linux host (≥ 8 cores, root, ideally Intel RDT) with a **hardware-timestamping NIC with AF_XDP zero-copy** (Intel i210/i226, X710 or E810), two if possible | Headline latency numbers, PTP, zero-copy AF_XDP, noisy-neighbor experiments |
| **ADS-B receiver:** RTL-SDR (v4) with a 1090 MHz antenna, or a better SDR; ideally a **GPS-disciplined reference** | Real IQ recordings, demodulator sensitivity, the frequency study |
| **OpenSky Network research access** | Real traffic for decoder agreement and large replays |
| **Exchange data:** Nasdaq ITCH 5.0 files or pcaps | Workstream J |

---

*IE 421 High-Frequency Trading. Group 19 project extension, building on the Fall 2025 ADS-B Aircraft Tracking System. Advised by Professor David Lariviere.*
