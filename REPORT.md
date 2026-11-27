# ADS-B as a Market Data Feed: Final Report

IE 421 (High-Frequency Trading), University of Illinois Urbana-Champaign, Group 19. Advisor: Prof. David Lariviere.

> **Status: skeleton.** This is the structure the team fills in for the final report. Results already in the repository are filled in, each with its source file and where it was measured. **Laptop** means an Apple M2 laptop (macOS, or Linux in Docker Desktop's VM), usually under background load: medians are indicative, tails are not. Placeholders:
> - `TODO (benchmark host)`: rerun on the Linux benchmark host (`bench/lab/run.sh`);
> - `TODO (real recordings)`: needs RTL-SDR recordings or OpenSky data;
> - `TODO (write)`: prose still to write.
>
> How to reproduce every number: [REPRODUCING.md](REPRODUCING.md). The plan this report follows: [README_UPDATES.md](README_UPDATES.md).

## Contents

1. [Abstract](#1-abstract)
2. [Motivation](#2-motivation)
3. [Goals, targets and scope](#3-goals-targets-and-scope)
4. [Baseline: the Fall 2025 system](#4-baseline-the-fall-2025-system)
5. [Architecture](#5-architecture)
6. [Workstreams](#6-workstreams)
7. [Measurement](#7-measurement)
8. [Correctness](#8-correctness)
9. [Limitations and future work](#9-limitations-and-future-work)
10. [Team and timeline](#10-team-and-timeline)
- [Appendix A: demo video outline](#appendix-a-demo-video-outline)
- [Appendix B: sources](#appendix-b-sources)

---

## 1. Abstract

`TODO (write)`, about 150 words, once the benchmark-host numbers exist. Points to cover:
- the aircraft feed (ADS-B, 1090 MHz) treated as exchange market data: decode into a book, publish a sequenced feed with gap recovery, push to browsers;
- the Fall 2025 system as the baseline, measured on the same traffic;
- the headline result: frame → map in the browser, Fall 2025 1.10–1.59 s vs new 69–88 ms (median, laptop; `TODO (benchmark host)`), and 17% → 76–78% of positions reaching the map;
- the verification approach (TLA+, GenMC, deterministic simulation, fuzzing, differential oracles against readsb);
- what remains (hardware-dependent numbers, ITCH, camera fusion).

## 2. Motivation

`TODO (write)`. Material from README_UPDATES.md (introduction, §1):
- **Why an aircraft feed stands in for market data.** Aircraft broadcast position, altitude and speed many times a second. Like an exchange feed it is a high-rate stream of small fixed-format messages about many instruments (aircraft), from which a consumer maintains a book (the track table) and fans updates out to subscribers. The same pressures apply: latency, loss, redundant A/B lines, slow consumers, overload, and correctness under all of them.
- **Owning the whole path.** In the Fall 2025 system almost all latency was outside the code one would normally optimize (README_UPDATES.md §1, *Where the time actually goes*):

| Segment | Approximate latency | Controlled by (Fall 2025) |
|---|---|---|
| RF → USB transfer → SDR sample buffer | 10–60 ms (librtlsdr default 256 KB ≈ 55 ms at 2.4 MS/s) | librtlsdr defaults |
| Demodulation and output batching (dump1090/readsb) | 1–100+ ms | another process |
| Decode → state → publish | 1–10 µs | us |
| Database write and poll | 1,000–2,000 ms | Fall 2025 design |

- **Generality.** The same infrastructure is meant to carry Nasdaq ITCH 5.0 (Workstream J, not started).

## 3. Goals, targets and scope

### 3.1 Targets and where we stand

Targets from README_UPDATES.md §2. "Measured so far" is from the repository's docs; none of it is from the benchmark host yet.

| Metric | Target (x86-64 benchmark host) | Measured so far | Where | Source |
|---|---|---|---|---|
| Demodulator cost | ≤ 10 cycles/sample (AVX2) | 6.07 ns/sample at 2.4 MS/s, one thread (≈ 3 ns needed) | Laptop | core/README.md *Demodulator* |
| Demodulator sensitivity | ≥ 90% of readsb's valid frames on the same IQ recording | 1–20% more messages than readsb (+10% on weak signals), synthetic IQ | Synthetic | core/README.md |
| Sample → wire (preamble sample → NIC TX timestamp) | p99 < 5 ms | IQ sample → track state p50 5.2 ms, p99 9.2 ms (6.8 ms blocks); p50 1.2 ms with 1.7 ms blocks. No NIC timestamps | Laptop, synthetic IQ | core/README.md *IQ into the live pipeline* |
| Decode + state update, per frame | p99 < 300 ns | frame in → state updated p50 0.38 µs, p99 1.8 µs at 65k frames/s | Laptop | core/README.md *Indicative numbers* |
| Frame ready → `sendmsg` returns (kernel UDP) | p99 < 10 µs | `send()` p50 750–1,459 ns (medians only) | Laptop VM | core/README.md |
| AF_XDP vs kernel UDP | ≥ 2× lower p99 TX | copy mode equals kernel UDP on send; receive p50 1.71 → 1.13 µs | Laptop VM, veth | core/README.md |
| Shared-memory IPC | p99 < 500 ns | p99 209 ns at 100k msg/s (ring alone) | Laptop | core/README.md |
| Pipeline throughput under replay | ≥ 2 M frames/s | core replay 1.9–2.0 M frames/s; `adsb-live` 1 M frames/s with publishing | Laptop | core/README.md *Overload semantics* |
| ITCH L3 book build | ≥ 20 M msg/s per core | not started | | |
| Frame → browser render | < 100 ms | 69–88 ms median (estimated; render itself not measured) | Laptop, Docker | bench/results/legacy-baseline/README.md |

Final column for the report: `TODO (benchmark host)` for every row.

### 3.2 Scope tiers and status

Tiers from README_UPDATES.md §3; status from TEAM.md.

| Tier | Item | Status |
|---|---|---|
| Must | A: SIMD demodulator | Built, on synthetic IQ; SIMD (NEON/SSE2), 10-cycle target not met; real recordings `TODO (real recordings)` |
| Must | C: decoder | Built; TC 29/31, Comm-B, optional single-bit repair; agreement on real traffic `TODO (real recordings)` |
| Must | D: track state | Built |
| Must | E: multicast feed with gap recovery (kernel UDP) | Built |
| Must | F: overload semantics | Built and tested to 10× saturation |
| Must | G: record/replay/measurement with baselines | Built; before/after benchmark on the laptop; final numbers `TODO (benchmark host)` |
| Should | B: A/B arbitration | Built, on simulated and synthetic IQ lines; real two-receiver numbers need a second RTL-SDR |
| Should | E: AF_XDP | Built, both sides, copy mode; zero-copy `TODO (benchmark host)` |
| Should | E: hardware timestamps and PTP | Not built (on hold) |
| Should | E: shared-memory IPC | Built |
| Should | F: conflated feed | Built |
| Should | H: TLA+ and memory-model testing | Built |
| Should | I: noisy-neighbor isolation with minimal camera fusion | Not started |
| Could | J: ITCH 5.0 L3 book | Not started |
| Could | E: io_uring transport | Built (publisher side) |
| Could | H: deterministic simulation testing | Built, per-push and nightly in CI |
| Added (§22) | IQ scenario generator for testing SDRs | Built and checked against readsb and the MathWorks receiver on synthetic IQ |
| Added (§22) | Transponder frequency-stability study | Method built and checked on synthetic IQ; the study `TODO (real recordings)` |

## 4. Baseline: the Fall 2025 system

**System.** RTL-SDR → dump1090 → SBS-1 text → Python streamer (100-record batches, 1 s flush) → Supabase Postgres → Node/Express → browser polling every 2 s (README_UPDATES.md §1; [FALL2025_README.md](FALL2025_README.md), [PROJECT_REPORT.md](PROJECT_REPORT.md)).

**How it was measured.** `bench/legacy/` runs the system as delivered (git tag `fall2025-final`) in Docker on synthetic traffic with ground truth, through readsb v3.16.21, and matches every shown position to the frame it came from (bench/legacy/README.md). Postgres is local, so latencies are a lower bound on the deployed system.

**Measured problems** (bench/results/legacy-baseline/README.md, laptop, 50–6,000 aircraft):

| Problem | Measurement |
|---|---|
| Latency designed in: batch flush, database round trip, 2 s poll | Frame → visible on the map 1.07–1.57 s median, 2.05–2.88 s p99 |
| Most data discarded: one SBS message per aircraft per second, of any type | 17% of transmitted positions reach the database |
| Stale and inaccurate map | Shown position 1.8–2.2 s old (median, p99 about 12.5 s); 454–517 m from the aircraft's real position (median) |
| Throughput was not the problem | At 6,000 aircraft (24,200 frames/s): streamer 25% of a core, no backlog |
| Committed schema incomplete | `aircraft_position_history` missing from `ads-b-receiver/schema.sql`: a fresh deployment fails every batch insert |

`TODO (benchmark host)`: rerun `bench/legacy/run.sh` (lab step `legacy`) for the final before/after table.

## 5. Architecture

`TODO (write)`: a diagram of what was built, from README.md *How it fits together* and README_UPDATES.md §4, marking what is built and what is planned.

```
receiver (RTL-SDR) → readsb ──Beast TCP──▶ adsb-live: decode + track state (C++)
  or IQ file/stdin → adsb-live --iq (own demodulator; A/B: --b-port / --iq-b)
                                              │  sequenced feed: kernel UDP multicast, io_uring, AF_XDP, or shared memory
                                              │  + retransmit (UDP) and snapshot (TCP) servers for gap recovery
                                              │  + optional conflated feed (--conflate)
                                              ▼
                       subscribers (C++ adsb-feed-cli, shared-memory readers)
                       adsb-gateway (Node) ──WebSocket, conflated per client──▶ 2D map and 3D globe (ads/)
```

Topics to cover:
- hot-path rules (README_UPDATES.md §4): one pinned thread per stage, SPSC rings, no allocation after startup, no locks or exceptions, fixed-size POD messages, event time instead of wall time;
- the feed protocol (MoldUDP64-style; [protocol/feed_spec.md](protocol/feed_spec.md)): one JSON schema generating C++, JavaScript and Python, 72-byte `TrackUpdate` in feed v1;
- the naive baseline build (`naive-baseline` preset): the same code with mutex queues and `std::unordered_map`;
- planned and not built: live `librtlsdr` source, hardware timestamps, camera fusion, ITCH.

## 6. Workstreams

Each subsection: what was built, how it was verified, results, limitations. Owners from [TEAM.md](TEAM.md).

### 6.1 Workstream A: RF ingest, IQ generator and demodulator

**Built.** `adsb-iqsynth` (synthetic 1090 MHz IQ with Doppler, multipath, the radio horizon, collisions and receiver impairments; SigMF with per-message truth); `adsb-demod` (gate, preamble at 5 phases, half-bit slicer plus a Viterbi sequence detector, matched-filter timing; four-wide SIMD, output byte-identical to the scalar version); `adsb-live --iq` (paced IQ reader, block ring that drops and counts whole blocks). Source: core/README.md *Synthetic IQ*, *Demodulator*, *IQ into the live pipeline*.

**Verified.** `test_iq.cpp`, `test_demod.cpp`, `test_iq_source.cpp`; pinned cross-platform hashes; `fuzz_demod`; `check-iq.sh`, `check-demod.sh`, `check-iq-live.sh` in CI job `iq`, scored against the generator's truth and pinned readsb; independent reviews and mutation testing (core/README.md *How correctness is checked*).

| Result | Value | Where | Source |
|---|---|---|---|
| readsb on the generator's files, clean 15–30 dB | 98.8% decoded, timestamps within 0.28 µs | Synthetic | core/README.md |
| Ours vs readsb, messages (busy / weak / impaired / multipath) | 6,332 / 6,257; 1,136 / 1,029; 2,462 / 2,391; 1,364 / 1,134 | Synthetic | core/README.md |
| DF17 matched, baseline synthetic IQ | adsb-demod 2,746; MathWorks 2,571; truth 3,417 | Synthetic | [MATLAB comparison](bench/matlab/README.md) |
| MATLAB weak-signal misses with no accepted preamble within 1.67 µs of truth | 8,677 of 9,413 across three seeds | Synthetic | [MATLAB diagnostics](bench/matlab/README.md#diagnostic-results) |
| Start-time error | ≤ 0.14 samples (0.06 µs); readsb 0.54 | Synthetic | core/README.md |
| Speed, 2.4 MS/s, one thread | 6.07 ns/sample (69× real time); readsb 5.35 ns | Laptop | core/README.md |
| IQ sample → track state, 1.7 / 6.8 / 27.3 ms blocks | p50 1.2 / 5.2 / 20 ms | Laptop | core/README.md |
| Sensitivity vs readsb on a real recording | `TODO (real recordings)` | | |
| USB transfer-size sweep on a real RTL-SDR | `TODO (real recordings)` | | |
| Speed in cycles/sample on AVX2 | `TODO (benchmark host)` | | |

**Limitations.** The sensitivity results use synthetic IQ. The live `librtlsdr` path still needs a physical dongle run, and the 10-cycle target needs a benchmark-host measurement. The generator includes Mode A/C replies. Aircraft antenna patterns and terrain remain open. The MATLAB comparison covers DF17, and the stock example exceeded its candidate capacity on the 600-aircraft case.

### 6.2 Workstream A (study): transponder frequency stability

**Built.** `adsb-carrier` (per-message carrier estimate once a message is decoded, Doppler from the decoded track) and `core/scripts/carrier-study.py` (receiver error and drift, per-aircraft offset, drift and Allan deviation, groups by type). Source: core/README.md *Transponder frequency study*.

**Verified.** `check-carrier.sh` (11 checks, CI job `iq`) against generator scenarios with known offsets, drifts, Doppler and receiver error.

| Result | Value | Where | Source |
|---|---|---|---|
| Per-message frequency error | p50 23 Hz, p95 117 Hz | Synthetic | core/README.md |
| Per-aircraft offset / drift error | p50 6 Hz / 0.08 Hz/s | Synthetic | core/README.md |
| Doppler error | p50 0.4 Hz | Synthetic | core/README.md |
| Offsets and drifts by aircraft type | `TODO (real recordings)` | | |

**Limitations.** Absolute offsets need a GPS-disciplined or calibrated receiver; multipath is the largest error; phase-incoherent transmitters are listed, not measured.

### 6.3 Workstream B: A/B line arbitration

**Built.** `Arbiter` (`core/include/adsb/ingest/arbiter.hpp`): first copy of each frame forwarded, per-line contribution, win margin, relative radio clock; in `adsb-live` (`--b-port`, `--iq-b`) and `adsb-replay --arbitrate`. Source: core/README.md *A/B line arbitration*.

**Verified.** `test_arbiter.cpp` (ground truth, brute-force model), `check-ab.sh` (22 checks, CI job `ab`), review and mutation testing.

| Result | Value | Where | Source |
|---|---|---|---|
| Two synthetic IQ receivers | 744 copies matched exactly; union 1,138 frames, 16% over the better line | Synthetic | core/README.md |
| Relative radio clock | +20.000 ppm recovered (true +20) | Synthetic | core/README.md |
| Cost | about 25 ns per frame | Laptop | core/README.md |
| Two real receivers: contribution, win margin, clock | `TODO (real recordings)`: needs a second RTL-SDR | | |

**Limitations.** Linear clock model (no temperature wander); no real two-receiver data.

### 6.4 Workstream C: Mode S decoder

**Built.** DF0/4/5/11/16/17/18/20/21/24, ADS-B TC 0–22, 28, 29, 31, Comm-B registers 1,0 1,7 2,0 3,0 4,0 5,0 6,0 (inferred), optional single-bit repair (`--fix`). Source: core/README.md.

**Verified.** Published vectors (*The 1090 MHz Riddle*, pyModeS), synthetic frames for every format, `fuzz_decoder`, an independent review against pyModeS and readsb, mutation testing (40/40). Position-by-position comparison with readsb: `bench/legacy/compare-positions.sh`.

| Result | Value | Where | Source |
|---|---|---|---|
| Decode one DF17 frame | 22 ns | Laptop | core/README.md |
| Positions vs readsb (1,000 aircraft, 60 s) | 0.9964 of readsb's; 0.9994 with `--fix`; 0 unexplained | Synthetic | bench/legacy/README.md |
| Agreement with readsb on real traffic (target ≥ 99.9%) | `TODO (real recordings)` | | |

**Limitations.** TC 29 subtype 0, DF19 and other Comm-B registers not decoded; Comm-B values not used by the track engine; GNSS-height unit question open (core/README.md *Limits*).

### 6.5 Workstream D: track state engine

**Built.** Open-addressing track store (64-byte hot record, backward-shift deletion), CPR pairing, duplicate filter, out-of-order rejection, plausibility check, timing-wheel expiry; `nic`, heading and airspeed for feed v1. Source: core/README.md.

**Verified.** Randomized differential test against `std::unordered_map`; end to end against the generator's ground truth (zero phantom tracks, positions within 12 m); determinism check across builds and compilers.

| Result | Value | Where | Source |
|---|---|---|---|
| Track lookup, 4,000 aircraft | flat 2.4 ns vs `unordered_map` 8.1 ns | Laptop | core/README.md |
| Insert + erase churn | flat 19–28 ns vs 190–210 ns | Laptop | core/README.md |
| Full engine per frame, 300 aircraft | 118 ns flat vs 124 ns naive | Laptop | core/README.md |
| Same at 10,000 aircraft | `TODO (benchmark host)` | | |

**Limitations.** NACp, SIL and NUCp decoded but not carried on the feed.

### 6.6 Workstream E: feed distribution and transports

**Built.** Publisher (sequencing, batching while busy, heartbeats, end of session), retransmit and snapshot servers, `SubscriberCore` / `SubscriberClient`; transports: kernel UDP, io_uring (publisher), AF_XDP (both sides, copy mode, XDP program loaded without libbpf), shared-memory ring. Source: core/README.md, protocol/feed_spec.md.

**Verified.** TLA+ model (`spec/`), unit tests per subscriber rule, `check-feed.sh` over every transport, `check-shm.sh`, GenMC models of the shm slot protocol, deterministic simulation (§8), fuzzing.

| Result | Value | Where | Source |
|---|---|---|---|
| Publish → applied, UDP loopback unicast | p50 49 µs, p99 79 µs | Laptop | core/README.md |
| Shared-memory ring, write → read | p50 125 ns, p99 209 ns | Laptop | core/README.md |
| `send()` p50: kernel / io_uring SQPOLL / no SQPOLL | 1,459 / 208 / 1,625 ns | Laptop VM | core/README.md |
| Receive over veth: kernel `recv()` / AF_XDP, send → recv p50 | 1.71 / 1.13 µs | Laptop VM | core/README.md |
| All transports, same histograms, pinned cores | `TODO (benchmark host)` (template in §7.4) | | |
| AF_XDP zero-copy | `TODO (benchmark host)` | | |

**Limitations.** No hardware timestamps or PTP; IPv4 only; one AF_XDP subscriber per device queue; blocking snapshot fetch in the C++ subscriber (core/README.md *Limits*).

### 6.7 Workstream F: overload semantics and conflation

**Built.** A defined policy at each boundary (drop and count at the input, lossless inside, never block on the network, conflation toward browsers); `--overload`, `--max-queue`; publisher batching while busy; the conflated feed (`--conflate`). Source: core/README.md *Overload semantics*.

**Verified.** `check-overload.sh` (25 checks: 0.5×, 2×, 10× saturation and a burst), `check-conflate.sh` (23 checks), CI jobs `overload`, `conflate`.

| Result | Value | Where | Source |
|---|---|---|---|
| Loss at 0.5× / 2× / 10× saturation | 0% / 50% / 90%, all at the input boundary and counted | Laptop | core/README.md |
| `adsb-live` with publishing, batching while busy | keeps up at 1 M frames/s (queue wait p99 24 µs); 4.4% dropped and counted at 2 M | Laptop | core/README.md |
| Conflated feed at 10 Hz | carries 96% of the full feed's updates on ADS-B traffic (it caps per-aircraft rates more than it saves bandwidth) | Laptop | gateway/README.md *Limits* |
| Saturation points on the host | `TODO (benchmark host)` | | |

**Limitations.** No live SDR source behind the IQ ring yet; `adsb-live`'s state thread spins when idle.

### 6.8 Workstream G: record, replay and measurement

**Built.** `.adsbrec` recording format, deterministic replay (inline and two-thread), synthetic traffic with ground truth (`adsb-synth`), the naive baseline build, latency histograms, the before/after harness (`bench/legacy/`), the benchmark-host suite (`bench/lab/run.sh`). Source: core/README.md, bench/legacy/README.md, bench/lab/README.md.

**Verified.** `check-determinism.sh` across speeds, threading, naive build and compilers (CI job `determinism`).

| Result | Value | Where | Source |
|---|---|---|---|
| Frame → map in the browser, Fall 2025 vs new (est.) | 1.10–1.59 s vs 69–88 ms | Laptop, Docker | bench/results/legacy-baseline/README.md |
| Frame → WebSocket at the browser | 4.8–30 ms (mostly readsb's output buffer) | Laptop | same |
| Positions reaching the map | 17% vs 76–78% | Laptop | same |
| Map error vs truth | 461–540 m vs 66–69 m | Laptop | same |
| Receive → track state inside `adsb-live` | 33–67 µs p50 | Laptop | same |
| Tails (p99, p99.9) | unreliable on the laptop (host stalls detected) | | bench/results/legacy-baseline/report.md |
| Final before/after table | `TODO (benchmark host)` | | |

**Limitations.** The browser columns are estimates (render not measured); readsb stays in the "after" path until the demodulator replaces it live; the committed sweep ran `adsb-live` with the old one-datagram-per-update default.

### 6.9 Workstream H: correctness and verification

Summary in §8.

### 6.10 Workstream I: noisy neighbor and camera fusion

Not started. Planned (README_UPDATES.md §13): the interference matrix in §7.6, and a minimal fusion pipeline consuming the conflated feed. Needs the benchmark host (ideally with Intel RDT) and a camera at a site.

### 6.11 Workstream J: Nasdaq ITCH 5.0 L3 book

Not started. Planned (README_UPDATES.md §14): a full-day ITCH file through our MoldUDP64 stack, book invariants, ≥ 20 M msg/s per core. Needs only public sample files.

### 6.12 Web: gateway, 2D map and 3D globe

**Built.** `gateway/` (Node 22, no dependencies): JS port of the subscriber, per-client conflated fan-out with credit flow control, the Fall 2025 REST API from memory. The Fall 2025 map (`ads/`) kept and extended: both the 2D map and the 3D globe take the live feed through `ads/live-feed.js`. Source: gateway/README.md.

**Verified.** Node tests (subscriber rules and simulation, WebSocket framing, fan-out, flow control, `live-feed.js` against a real gateway); `check-gateway.sh` (26 checks, CI job `gateway`); mutation testing (30/31).

| Result | Value | Where | Source |
|---|---|---|---|
| Publish → gateway book | p50 40 µs | Laptop | gateway/README.md |
| Publish → WebSocket client (Node) | p50 95 µs, p99 about 2 ms | Laptop | gateway/README.md |
| Slow client (3 ms per frame), window 8 vs no flow control | p50 19–25 ms vs > 1 s behind (max 18.6 s) | Laptop | gateway/README.md |
| Map, 150 aircraft | markers 0 m from the latest position, one render 8 ms | Laptop | gateway/README.md |
| Render to pixels, frame rate | `TODO (benchmark host)`: not measured | | |

**Limitations.** No TLS or authentication; no Postgres history in the gateway (cones and collisions use its 200-point trails); tiles need internet access.

## 7. Measurement

### 7.1 Method

`TODO (write)`, from README_UPDATES.md §11.3: timestamp points, histograms (p50, p90, p99, p99.9, p99.99, max), coordinated omission (latency from the intended send time), warm-up, ≥ 5 runs per configuration, pinned frequency, isolated cores, host configuration recorded with every result. Describe the host: `TODO (benchmark host)` (from `host.json` of the lab run).

### 7.2 Tail analysis

Every p99.9 and max value explained, not just reported (README_UPDATES.md §11.6). Tools: `perf`, `perf sched`, `ftrace`, `bpftrace`, `/proc/interrupts`. `TODO (benchmark host)`.

| Stage | p50 | p99 | p99.9 | max | Outlier class | Cause (evidence) | Fix or mitigation |
|---|---|---|---|---|---|---|---|
| | | | | | | | |

### 7.3 PGO and ablation

Each optimization turned off one at a time (README_UPDATES.md §11.5), plus profile-guided optimization. `TODO (benchmark host)`.

| Configuration | Decode + state per frame (p50 / p99) | Replay throughput (frames/s) | Cycles/frame | Δ vs full build |
|---|---|---|---|---|
| Full build | | | | |
| + PGO | | | | |
| Naive queues (mutex) | | | | |
| Naive table (`unordered_map`) | | | | |
| Unpinned threads | | | | |
| … | | | | |

### 7.4 Transports on one histogram

From `bench/lab/run.sh` steps `transports` and `xdp`, alternated round by round. `TODO (benchmark host)`.

| Transport | `send()` p50 / p99 | send → recv p50 / p99 / p99.9 | Loss | Cores busy |
|---|---|---|---|---|
| kernel `sendto()` → `recv()` | | | | |
| io_uring SQPOLL | | | | |
| io_uring, no SQPOLL | | | | |
| AF_XDP copy mode | | | | |
| AF_XDP zero-copy | | | | |
| AF_XDP receive | | | | |
| Shared-memory ring (same host) | | | | |

### 7.5 10,000-aircraft load

`adsb-synth --aircraft 10000` through replay, `adsb-live` and the gateway. `TODO (benchmark host)`.

| Stage | Frames/s | p50 | p99 | p99.9 | CPU | Drops |
|---|---|---|---|---|---|---|
| Replay, decode + state | | | | | | |
| `adsb-live` → feed | | | | | | |
| Feed → gateway → WebSocket | | | | | | |
| Browser render | | | | | | |

### 7.6 Noisy-neighbor isolation matrix

README_UPDATES.md §13.1. `TODO (benchmark host)` (needs RDT for the CAT/MBA rows).

| Configuration | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| Pipeline alone | | | | |
| + neighbor, no isolation | | | | |
| + core isolation | | | | |
| + CAT (L3 partition) | | | | |
| + MBA | | | | |
| + IRQ affinity | | | | |

## 8. Correctness

Source: core/README.md *How correctness is checked*, spec/README.md, spec/memory-model/README.md.

| Technique | Applied to | Result |
|---|---|---|
| TLA+ / TLC | Gap recovery: loss, duplication, reordering, NAK, snapshot | `MC.cfg` passes (1,457,453 states); the no-snapshot variant fails liveness as intended |
| GenMC (RC11) | SPSC ring and shm slot protocol | 6 correct models verify, 20 mutants caught; found a lap-resume bug (fixed) |
| Stress on hardware | Both rings, x86 and ARM | 60 s per CI run, 0 violations |
| Deterministic simulation | Publisher, recovery, subscribers on a simulated network and clock | 200,000 quick and 5,000 deep worlds pass; 40,000 new seeds per CI run, nightly sweep; found four subscriber defects (fixed in C++ and JS) |
| Fuzzing | Beast, decoder, packets, feed, demodulator, carrier | libFuzzer in CI, 60 s per target |
| Differential oracles | Decoder and demodulator vs readsb; IQ generator vs readsb | See §6.1, §6.4 |
| Determinism hash | Full replay across speeds, threading, builds, compilers | CI job `determinism` |
| Sanitizers | Whole test suite | ASan + UBSan, TSan, GCC and Clang, `-Werror` |
| Mutation testing | Most modules | Planted bugs caught by the tests (counts per module in core/README.md) |
| Independent review | Each substantial change | Real bugs found and fixed (listed in core/README.md) |

`TODO (write)`: the bugs these methods found that ordinary tests did not, as the report's evidence that the methods pay off.

## 9. Limitations and future work

`TODO (write)`. Collect the per-workstream limitations above, then:
- hardware still needed (REPRODUCING.md *What needs hardware*): benchmark host, zero-copy NIC, RTL-SDR recordings, a second receiver, a camera;
- not started: hardware timestamps and PTP (on hold), camera fusion (I), ITCH (J);
- stretch goals (README_UPDATES.md §15): backtester, FPGA, multilateration (the arbiter's relative clock is groundwork).

## 10. Team and timeline

### 10.1 Team and roles

From [TEAM.md](TEAM.md).

| Role | Owner(s) | Workstreams |
|---|---|---|
| 1. RF and demodulation | Aniket (`S-Anikett`), Philip (`philipwestling`) | A, B |
| 2. Decoder, track state, exchange data | Daniel (`dsiegal11`) | C, D, J |
| 3. Feed transports and verification | Ryan (`ryan-air`) | E, F, H |
| 4. Measurement, isolation, UI, report | open | G, I, M7–M8 |

Items taken across roles are marked in TEAM.md (for example "**Done (Ryan):**"). `TODO (write)`: who built what, from TEAM.md and the merged pull requests.

### 10.2 Milestones

From README_UPDATES.md §19. `TODO (write)`: status per milestone at submission.

| Milestone | Exit criteria (plan) | Status |
|---|---|---|
| M0 Foundations | CMake, CI, SPSC ring, clock, histogram; hardware checks; feed spec | |
| M1 Demodulator + decoder | ≥ 90% of readsb on IQ recordings; USB sweep; decoder ≥ 99.9% agreement; fuzzers in CI | |
| M2 Track state + A/B | Open-addressing table with expiry; two-line arbitration live | |
| M3 Feed (kernel UDP) | Publisher, recovery servers, feed-cli, gateway; TLA+ checks | |
| M4 Measurement + overload | Determinism hash in CI; 10k aircraft; naive baseline; overload 2× and 10×; conflated feed; first benchmark report | |
| M5 Transports and isolation | AF_XDP, shared memory, hardware timestamps; noisy-neighbor matrix; minimal fusion | |
| M6 ITCH L3 book | Full-day file through MoldUDP64; invariants; throughput | |
| M7 Tuning and tail analysis | PGO and ablation; every p99.9 class explained; targets met or gap explained; DST nightly | |
| M8 Final | Final report, demo video, reproducible instructions | |

---

## Appendix A: demo video outline

About 5–7 minutes. All shots use tools on `main`; commands from README.md, gateway/README.md and core/README.md. Run from the repository root after `cd core && cmake --preset release && cmake --build --preset release && cd ..`. `TODO`: rehearse every shot and record on one machine.

| # | Shot | What is on screen | Commands |
|---|---|---|---|
| 1 | The problem | The Fall 2025 map and the before/after table | `bench/results/legacy-baseline/report.md` (no live run) |
| 2 | Synthetic traffic | `adsb-synth` writing 15 minutes of 150 aircraft over Chicago, with ground truth | `core/build/release/tools/adsb-synth -o /tmp/demo.adsbrec --aircraft 150 --duration 900 --receiver 41.88,-87.63` |
| 3 | Gateway | The gateway starting, waiting for the feed | `node gateway/bin/adsb-gateway.mjs --feed 127.0.0.1:41900 &` |
| 4 | Publish in real time | `adsb-publish` replaying the recording onto the feed | `core/build/release/tools/adsb-publish /tmp/demo.adsbrec --feed 127.0.0.1:41900 --bind 127.0.0.1 --speed 1 --busy-wait` |
| 5 | 2D map | Aircraft moving live, no polling; counters | http://127.0.0.1:8080/ and http://127.0.0.1:8080/gateway/stats |
| 6 | 3D globe | The same feed on the Cesium globe; follow mode | http://127.0.0.1:8080/3d.html |
| 7 | Loss and recovery | Rerun the publisher with injected loss; the map stays correct; the publisher's and the gateway's book digests match | add `--drop-rate 0.01` to shot 4; start the gateway with `--until-end` so it prints its digest on exit |
| 8 | IQ generator | A SigMF recording with per-message truth | `core/build/release/tools/adsb-iqsynth -o /tmp/scene --aircraft 150 --duration 30` |
| 9 | Our demodulator | Frames out of raw samples, scored against the truth; throughput on stderr | `core/build/release/tools/adsb-demod /tmp/scene.sigmf-meta --beast /tmp/scene.beast`, then `python3 core/scripts/iq-score.py /tmp/scene.sigmf-meta /tmp/scene.beast` |
| 10 | IQ to map (optional) | The whole pipeline from samples to the browser | generate with `--receiver 41.88,-87.63` so the aircraft are in the map's view, then `core/build/release/tools/adsb-live --iq /tmp/scene.sigmf-meta --publish 127.0.0.1:41900` with the gateway of shot 3 |
| 11 | Verification | CI jobs green; one TLC run; one `adsb-dst` sweep | `bash spec/run-tlc.sh`; `core/build/release/tools/adsb-dst --seeds 1000` |
| 12 | Results | The before/after table, then the benchmark-host tables of §7 | `TODO (benchmark host)` |

Shot 10 is composed from documented options but has not been rehearsed: check it first.

## Appendix B: sources

| Topic | File |
|---|---|
| Plan, goals, tiers, timeline | [README_UPDATES.md](README_UPDATES.md) |
| Roles and status | [TEAM.md](TEAM.md) |
| How to reproduce every number | [REPRODUCING.md](REPRODUCING.md) |
| C++ pipeline, results, correctness | [core/README.md](core/README.md) |
| Feed protocol | [protocol/feed_spec.md](protocol/feed_spec.md) |
| TLA+ model | [spec/README.md](spec/README.md) |
| GenMC models | [spec/memory-model/README.md](spec/memory-model/README.md) |
| Gateway | [gateway/README.md](gateway/README.md) |
| Before/after method | [bench/legacy/README.md](bench/legacy/README.md) |
| Before/after results | [bench/results/legacy-baseline/README.md](bench/results/legacy-baseline/README.md), [report.md](bench/results/legacy-baseline/report.md) |
| Benchmark-host suite | [bench/lab/README.md](bench/lab/README.md) |
| Fall 2025 system | [FALL2025_README.md](FALL2025_README.md), [PROJECT_REPORT.md](PROJECT_REPORT.md) |
