# ADS-B as a Market Data Feed — Project Updates

This document describes how we are extending the Fall 2025 **ADS-B Aircraft Tracking System** (see [README.md](README.md) and [PROJECT_REPORT.md](PROJECT_REPORT.md)) into a low-latency, HFT-style system. The original README still documents the existing system. This file documents **what we are changing, how, and how we will measure it**.

> **Core idea:** treat the 1090 MHz aircraft feed as a stand-in for an exchange market data feed. An aircraft is an "instrument", each Mode S frame is a "market data message", and the per-aircraft track table is the "order book". Everything we build (decoding, state, distribution, replay, and measurement) mirrors how a trading firm's feed handler is built and evaluated.

---

## Table of Contents

1. [Where We Are Starting From](#1-where-we-are-starting-from)
2. [Goals and Non-Goals](#2-goals-and-non-goals)
3. [Target Architecture](#3-target-architecture)
4. [Workstream A — Ingest](#4-workstream-a--ingest)
5. [Workstream B — Mode S Decoder](#5-workstream-b--mode-s-decoder)
6. [Workstream C — Track State Engine](#6-workstream-c--track-state-engine)
7. [Workstream D — Feed Distribution (UDP Multicast)](#7-workstream-d--feed-distribution-udp-multicast)
8. [Workstream E — Record, Replay, and Measurement](#8-workstream-e--record-replay-and-measurement)
9. [Workstream F — Camera Fusion](#9-workstream-f--camera-fusion)
10. [Stretch Goals](#10-stretch-goals)
11. [Repository Layout](#11-repository-layout)
12. [Toolchain and Dependencies](#12-toolchain-and-dependencies)
13. [Hardware and Test Environments](#13-hardware-and-test-environments)
14. [Timeline and Milestones](#14-timeline-and-milestones)
15. [Risks and Mitigations](#15-risks-and-mitigations)
16. [Open Decisions](#16-open-decisions)

---

## 1. Where We Are Starting From

The Fall 2025 system ([ads-b-receiver/adsb_streamer_optimized.py](ads-b-receiver/adsb_streamer_optimized.py), [ads/server.js](ads/server.js)) works like this:

```
dump1090 ──SBS-1 text (TCP 30003)──▶ Python streamer ──batch CSV COPY──▶ Supabase Postgres
                                                                              │
                         Browser ◀──HTTP poll every 2 s── Node/Express (2–3 s cache)
```

| Property | Fall 2025 system | Why it matters for this project |
|---|---|---|
| Input format | SBS-1 text, already decoded by dump1090 | We never see raw frames, CRC, or timing |
| Language | Python and Node.js | GC, interpreter overhead, allocation on every message |
| Buffering | 100-record batches, flushed every 1 s | Latency goes into the database by design |
| Distribution | Database plus HTTP polling | One-to-many fan-out goes through a database |
| End-to-end latency | About 1–2.2 s, frame to screen (report's data-flow example) | This is our "before" number |
| Measurement | Throughput only (ops/s, cache hit rate) | No per-stage latency and no tail analysis |
| Reproducibility | None. Live data only, with a mock generator | Results cannot be re-run |

The existing Kalman filter ([ads/kalman-filter.js](ads/kalman-filter.js)), probability cones, collision detection, and the Leaflet/Cesium frontend are reused. They become **subscribers** of the new feed instead of database readers.

---

## 2. Goals and Non-Goals

### Goals
1. **A C++ hot path** (ingest → decode → track state → publish) with no heap allocation, no locks, and no syscalls other than the socket read and send.
2. **A market-data-style feed**: fixed-size binary messages over UDP multicast with sequence numbers, gap detection, retransmission, and snapshots.
3. **Deterministic replay**, so every benchmark and bug can be re-run exactly.
4. **Rigorous latency measurement**: per-stage p50, p99, p99.9, and max, with naive-versus-optimized and old-versus-new comparisons, and an explanation of every tail outlier we can attribute.
5. **Camera fusion** as an independent subscriber that proves the feed's design: a slow consumer must not affect the fast path.

### Latency targets (initial, to be validated in Weeks 3–4)
| Path | x86 Linux benchmark host | Raspberry Pi 5 |
|---|---|---|
| Decode + state update, per frame | p99 < 1 µs | p99 < 5 µs |
| Ingest read → multicast send (in-process) | p99 < 10 µs | p99 < 50 µs |
| Sustained throughput (replay, max speed) | ≥ 1 M frames/s | ≥ 100 k frames/s |
| Frame received → browser render | < 100 ms (versus about 2 s today) | same |

For scale: a busy live receiver produces about 1,000–5,000 frames per second. The synthetic load generator (§8.4) is what pushes us to rates where design choices actually show up.

### Non-goals
- Replacing dump1090/readsb demodulation in the core project. That is the FPGA stretch goal.
- Kernel-bypass networking (DPDK, ef_vi). It is out of scope for our hardware, but we will discuss where it would help.
- Live trading of any kind. The market data and strategy stretch goals are **read-only feeds and paper backtests**.

---

## 3. Target Architecture

```
                         ┌──────────────────────── adsb-core (C++20, one process) ─────────────────────────┐
 RTL-SDR ─▶ readsb ──Beast─▶│ [Ingest]──SPSC──▶[Decode]──SPSC──▶[Track State]──SPSC──▶[Publish]             │
            (or recording)  │  core 1          core 2           core 2/3             core 3                 │
                            │   t0               t1               t2                   t3 ─▶ UDP send (t4)  │
                            │  └──────── per-stage timestamps → per-thread HDR histograms ────────┘         │
                            │  [Recorder] (tap on ingest; writes raw frames + rx timestamps to disk)        │
                            └───────────────────────────────────────────────┬────────────────────────────────┘
                                                                            │ UDP multicast 239.19.0.1:41900
                                                                            │ (incremental feed, MoldUDP64-style)
                 ┌────────────────────────────┬─────────────────────────────┼───────────────────────────┐
                 ▼                            ▼                             ▼                           ▼
        WebSocket gateway            Camera fusion (Python)         Archiver (optional)        CLI subscriber /
        (Node) ─▶ browser map        ─▶ fusion feed 239.19.0.2      ─▶ Postgres (history)      latency probe
                                                                    
        Gap recovery: Retransmit server (UDP unicast, last 65,536 msgs) + Snapshot server (TCP, full table)
```

**Design rules for the hot path**
- **One thread per stage, pinned** with `pthread_setaffinity_np` to cores isolated with `isolcpus=` and `nohz_full=`, CPU governor set to `performance`.
- **Lock-free SPSC ring buffers** between stages. Capacity is a power of two. Head and tail sit on separate 64-byte cache lines. Each side keeps a cached copy of the other side's index. Ordering uses `acquire`/`release` only. Messages are fixed-size PODs copied by value.
- **No allocation after startup.** All rings, the track table, and the send buffers are allocated and pre-faulted at init, then `mlockall(MCL_CURRENT | MCL_FUTURE)`. Enforced in debug builds by a `malloc` hook that aborts if called on a hot thread.
- **Busy-spin consumers**, with a `pause`/`yield` instruction in the spin loop. A configurable backoff mode allows a fair comparison on the Pi, which has few cores.
- **Event time, not wall time.** All state logic, including expiry and CPR pairing windows, uses the frame's own timestamp. This makes replay deterministic (§8.2).
- **Pluggable at both ends.** The frame source (live socket, recording file, load generator, later FPGA) and sink (multicast, null sink, in-memory capture for tests) are interfaces chosen at startup. The hot loop is templated on them, so there are no virtual calls per frame.

**Stage placement on a 4-core Raspberry Pi 5:** core 0 runs the OS, readsb, and IRQs. Core 1 runs Ingest. Core 2 runs Decode and Track State fused into one stage (benchmarked both fused and split). Core 3 runs Publish. On the x86 host, every stage gets its own isolated core.

---

## 4. Workstream A — Ingest

### 4.1 Frame source
- **Decoder:** **readsb** (preferred) or **dump1090-fa**, run only as a demodulator. We consume its **Beast binary output** on TCP port 30005. SBS-1 on port 30003 is no longer used.
- **Output-buffering trap:** both programs batch network output by default, and that batching can add **tens to hundreds of ms** of hidden latency. We will set the flush options to their minimums (`--net-ro-size` / `--net-ro-interval`) and measure the effect directly. This is our first "before/after" data point.
- **Socket setup:** `TCP_NODELAY`, a large `SO_RCVBUF`, non-blocking socket, and a busy-poll `recv` loop (`SO_BUSY_POLL` where supported).

### 4.2 Beast framing
```
0x1A  <type>  <6-byte 12 MHz timestamp, big-endian>  <1-byte signal>  <payload>
type '1' = Mode A/C (2 bytes)   '2' = Mode S short (7 bytes)   '3' = Mode S long (14 bytes)
Any 0x1A byte inside the frame is escaped as 0x1A 0x1A.
```
- The parser is a **resumable state machine** over the raw byte stream, because frames can be split across `recv` calls. It unescapes in place into a fixed `RawFrame` struct and never allocates.
- Malformed input (bad type, truncated escape) resynchronizes on the next `0x1A <valid type>` and increments a counter. It never throws.

### 4.3 Timestamps
Each frame carries two clocks:
| Clock | Source | Used for |
|---|---|---|
| `rx_ns` | `clock_gettime(CLOCK_MONOTONIC)` (or TSC/`CNTVCT_EL0`, calibrated) taken right after `recv` | In-pipeline latency, replay pacing |
| `beast_ts` | The 12 MHz counter readsb derives from the SDR sample count | Radio-time ordering, MLAT stretch goal, estimating radio → `recv` delay |

We will **evaluate mapping `beast_ts` to wall-clock time** by fitting a linear model (offset plus drift) of `beast_ts` against `rx_ns` over a sliding window. The residuals of that fit show the SDR-buffer and readsb-processing delay, which is the latency we cannot otherwise see. Note that the 12 MHz counter is free-running and **not** GPS-disciplined. Its crystal drifts by tens of ppm, which matters for the MLAT stretch goal (§10.5).

### 4.4 Deliverables
- `FrameSource` interface plus `BeastTcpSource`, `RecordingSource`, and `SyntheticSource`.
- Unit tests over captured byte streams that include split frames and escaped `0x1A` bytes.
- A fuzz target for the Beast parser.

---

## 5. Workstream B — Mode S Decoder

### 5.1 Scope
| Downlink Format | Content | CRC handling |
|---|---|---|
| **DF17** | ADS-B Extended Squitter (main target) | Parity must be 0. ICAO address is in the clear. |
| **DF18** | TIS-B / ADS-R / non-transponder | Parity must be 0 |
| **DF11** | All-call reply | Parity XOR interrogator ID; accept IID 0 only |
| DF4/5/20/21 | Surveillance altitude/identity | Address/parity: recover the ICAO from the CRC residual and accept it **only if the ICAO is already a known live track** |
| DF0/16 | ACAS | Parsed later if time allows |

DF17 type codes decoded:
| TC | Message | Fields |
|---|---|---|
| 1–4 | Identification | Callsign (6-bit charset, 8 chars), emitter category |
| 5–8 | Surface position | Surface CPR (local decode against receiver position), ground speed, track |
| 9–18 | Airborne position (baro altitude) | CPR lat/lon, 12-bit altitude (Q-bit 25 ft or Gillham 100 ft) |
| 19 | Airborne velocity | Subtypes 1/2 (ground speed, E/W and N/S components) and 3/4 (airspeed and heading), vertical rate |
| 20–22 | Airborne position (GNSS height) | As above, but geometric altitude |
| 28 / 29 / 31 | Status, target state, operational status | Squawk/emergency, selected altitude, NIC/NAC/SIL (used as position-quality metadata) |

### 5.2 Implementation details
- **CRC-24** (generator `0xFFF409`) uses byte-wise table lookup: one 256-entry `uint32_t` table, 1 KB, so it stays in L1. A slice-by-4 variant will be benchmarked. Single-bit error correction is **off by default**, because it trades false decodes for coverage. It can be switched on and its effect measured.
- **Bit extraction:** load the 112-bit message into two `uint64_t` registers and extract fields with constant shifts and masks. No byte-by-byte loops.
- **CPR decoding:**
  - **Global decode** needs an even/odd pair from the same aircraft received **within 10 s** (event time). The most recent pair is stored inside the track record (§6). The NL (number of longitude zones) lookup is a precomputed 59-entry latitude threshold table, so `acos`/`cos` is never called on the hot path.
  - **Local decode** against the aircraft's last known position, or the receiver location, handles single frames once a track has a position.
  - **Reasonableness check:** reject a position if the implied speed from the previous fix is above Mach 1 or the jump is larger than 180 NM. Count rejections.
- **Output:** a fixed-size `DecodedMsg` POD (ICAO, DF/TC, field bitmask, the field values, `rx_ns`, `beast_ts`). No strings anywhere.

### 5.3 Verification
1. **Known vectors:** the worked examples from *The 1090 MHz Riddle* (J. Sun) and the **pyModeS** test suite become unit tests.
2. **Differential oracle:** replay a recorded session into readsb's Beast input port (30004) and also into our decoder, then diff the decoded fields per frame (ICAO, callsign, altitude, velocity, lat/lon to 1e-5°). **pyModeS** is a second, independent oracle. Any mismatch fails CI.
3. **Fuzzing:** libFuzzer with ASan and UBSan on `decode(const uint8_t*, size_t)`, seeded with real frames. Invariants: no crash, no out-of-bounds read, a frame with bad CRC is never emitted as valid, and decoded values stay in physical ranges.
4. **Property tests:** encode → decode round trips for CPR and altitude across the full input range.

---

## 6. Workstream C — Track State Engine

### 6.1 Data structure
- An **open-addressing hash table** keyed by 24-bit ICAO. Capacity is a fixed power of two (default 8,192 slots, about 4× the largest realistic live count, so load factor stays at or below 0.25 live and 0.5 under synthetic load). It uses **linear probing**, a multiplicative (Fibonacci) hash of the ICAO, and **backward-shift deletion**, so there are no tombstones.
- **A cache-conscious record split:**
  - *Index array:* `uint32_t icao` plus `uint32_t slot` (8 B per entry, 8 entries per cache line). Probing touches only this array.
  - *Hot record:* exactly **64 bytes, cache-line aligned**. It holds lat/lon (int32 at 1e-7°), altitude, ground speed, track, vertical rate, last-update event time, a per-field "last updated" bitmask, and the outgoing sequence number.
  - *Cold record:* callsign, squawk, category, NIC/NAC, CPR even/odd pair buffers, and counters. It is touched only when those messages arrive.
- **Baseline for comparison:** `std::unordered_map<uint32_t, Track>` with the same API (node-based, with an allocation on insert). We also benchmark `absl::flat_hash_map` as a "good off-the-shelf" reference point.

### 6.2 Semantics
| Concern | Rule |
|---|---|
| **Duplicates** (same frame seen twice: readsb duplicates or multiple receivers) | A 64-entry ring per track of `(hash(payload), beast_ts)`. Drop if the same payload is seen within 2 ms event time. |
| **Out-of-order frames** | Each field group (position, velocity, identity) keeps its own event timestamp. An update older than the stored one is dropped for that field and counted. CPR pairing always uses the newest even and newest odd frame. |
| **Stale-track expiry** | A **timing wheel** with 1 s buckets and a 64 s horizon, driven by event time. Tracks with no update for 60 s are evicted and a `TrackDelete` message is published. Expiry costs O(1) per tick, with no full-table sweep. |
| **Change detection** | A `TrackUpdate` is published only when a field actually changes. The update bitmask tells subscribers which fields changed. |

### 6.3 Benchmarks
- Google Benchmark microbenchmarks: lookup hit, lookup miss, insert, and update, at 100 / 1k / 10k live tracks, with uniform and Zipf-skewed ICAO distributions (real traffic is skewed). Reported as ns/op and cache misses/op from `perf stat`.
- End-to-end: the state stage's latency histogram under replay at 1×, 100×, and max speed, compared against the `unordered_map` build.

---

## 7. Workstream D — Feed Distribution (UDP Multicast)

### 7.1 Framing: modeled on Nasdaq MoldUDP64
Using a real exchange framing scheme keeps us honest. It is also what the market-data stretch goal will reuse unchanged.

**Packet header (20 bytes)**
| Field | Type | Notes |
|---|---|---|
| `session` | char[10] | Changes on every publisher restart. A subscriber that sees a new session resets its state. |
| `seq` | u64 | Sequence number of the **first message** in this packet |
| `count` | u16 | Number of messages. `0` = heartbeat. `0xFFFF` = end of session. |

This is followed by `count` message blocks, each prefixed with `u16 length`. All integers are **little-endian**. MoldUDP64 itself is big-endian, but every machine we use is little-endian, so we deviate here on purpose and say so in the spec.

**`TrackUpdate` (64 bytes, fixed). Draft layout:**
| Offset | Field | Type | Unit |
|---|---|---|---|
| 0 | `msg_type` | u8 | `'U'` |
| 1 | `version` | u8 | 1 |
| 2 | `update_mask` | u16 | which fields changed |
| 4 | `icao` | u32 | lower 24 bits |
| 8 | `event_ts_ns` | u64 | frame rx time |
| 16 | `beast_ts` | u64 | 12 MHz counter |
| 24 | `lat_e7` / `lon_e7` | i32 ×2 | 1e-7 deg |
| 32 | `alt_ft` | i32 | ft (baro or GNSS, per a flag) |
| 36 | `gs_dkt` | u16 | 0.1 kt |
| 38 | `track_cdeg` | u16 | 0.01 deg |
| 40 | `vrate_fpm` | i16 | ft/min |
| 42 | `squawk` | u16 | BCD |
| 44 | `callsign` | char[8] | space-padded |
| 52 | `category`, `nic`, `flags` | u8 ×3 | |
| 55 | reserved | u8 | |
| 56 | `publish_ts_ns` | u64 | stamped right before `sendmsg`, for wire latency |

Other message types: `TrackDelete` (16 B), `Heartbeat` (as an empty packet), and later `FusionMatch` / `UnidentifiedObject` (from camera fusion) and `BookUpdate` (stretch). The spec lives in `protocol/feed_spec.md`. Matching struct definitions for **C++, JavaScript (DataView), and Python (`struct`/NumPy dtype)** are generated from one schema file so they cannot drift apart. A CI test checks that all three parse a golden packet identically.

### 7.2 Publishing policy
- A UDP payload of at most 1,472 bytes (so no IP fragmentation) holds 20 + 22 × (2 + 64) = **1,472 bytes, so 22 messages per packet**.
- **Flush policy** is configurable and part of the benchmark: `immediate` (one message per packet, lowest latency) versus `batch` (flush when full or after N µs). We will plot the latency/throughput tradeoff.
- Socket options: `IP_MULTICAST_TTL=1`, `IP_MULTICAST_LOOP` on (single-host testing), and a large `SO_SNDBUF`. Publishing must go over **wired Ethernet**. Wi-Fi multicast is rate-limited and lossy, and it will look like a gap-recovery bug.

### 7.3 Gap detection and recovery
1. A subscriber tracks `expected_seq`. On `seq > expected_seq` it buffers what arrived and sends a **retransmit request** (`session`, `seq`, `count`) by unicast UDP to the **retransmit server**. That server is a separate thread holding a ring of the last 65,536 messages (about 4 MB).
2. If the gap is older than the ring, the subscriber falls back to the **snapshot server** (TCP). It receives the full track table tagged "as of seq N", then applies buffered incrementals with `seq > N`. This is the same pattern exchanges use for late joiners.
3. Heartbeats (an empty packet carrying the next `seq`) are sent every 1 s, so a subscriber detects a gap even when traffic stops.
4. **Testing:** a `--drop-rate p` / `--drop-burst k` fault injector in the publisher, plus `tc netem` on Linux. The test asserts that after recovery, the subscriber's reconstructed table is byte-identical to the publisher's.

### 7.4 Subscribers
| Subscriber | Language | Purpose |
|---|---|---|
| `feed-cli` | C++ | Prints, measures one-way latency (`now − publish_ts_ns`), and verifies sequence numbers |
| **WebSocket gateway** | Node (`dgram` multicast) | Browsers cannot join multicast. The gateway joins the group and pushes binary frames to the existing Leaflet/Cesium UI over WebSocket, **replacing the 2 s database polling**. It throttles to the display rate for each browser independently. |
| Camera fusion | Python | §9 |
| Archiver (optional) | Python | Writes to the existing Postgres schema so history, statistics, and trails keep working |

---

## 8. Workstream E — Record, Replay, and Measurement

### 8.1 Recording format (`.adsbrec`)
```
File header: magic "ADSBREC1", version, receiver id, receiver lat/lon/alt, start wall-clock time,
             clock calibration (TSC frequency), build git hash
Records:     u64 rx_ns | u64 beast_ts | u8 type | u8 signal | u8 len | payload[len]   (packed, append-only)
Footer:      record count, CRC32 of file
```
- The recorder is a tap on the ingest stage through its own SPSC ring, so disk I/O never blocks the hot path. A full ring drops records and counts the drops, rather than applying backpressure.
- Replay reads the file through `mmap` with `MADV_SEQUENTIAL`.
- **Datasets to capture early:** a quiet hour, a peak hour, a 24-hour run, and sessions recorded alongside camera video (§9). Raw IQ samples (`rtl_sdr`) are also captured for a few minutes, which the FPGA stretch goal needs.

### 8.2 Deterministic replay
- Speeds: `1×` (original inter-arrival times), `N×` (scaled), and `max` (as fast as the pipeline accepts input).
- **Determinism contract:** for a given recording and build, the sequence of published messages, excluding the `*_ts_ns` wall-clock fields, is **byte-identical** across runs and replay speeds. This works because all state logic uses event time (§3). CI replays a reference recording and compares a SHA-256 of the output stream against a stored golden hash.

### 8.3 Measurement harness
- **Timestamp points:** t0 = after `recv`, t1 = frame parsed, t2 = decoded, t3 = state updated, t4 = after `sendmsg`. The timestamps travel with the message through the rings, so each stage boundary is measured on the same frame.
- **Histograms:** per-thread, fixed-memory, log-linear buckets (HdrHistogram style, 3 significant digits, 1 ns to 10 s). Recording costs no allocation and no shared cache lines. Histograms are merged when the run ends. Reported per stage and end to end: **p50, p90, p99, p99.9, p99.99, max**, plus throughput.
- **Coordinated omission:** in fixed-rate replay, latency is measured from the **intended** send time, not the actual one, so a stalled pipeline cannot hide its own stall.
- **Methodology:** warm up for 5 s, use ≥ 5 runs per configuration, fix CPU frequency, isolate cores, and record machine and kernel config in every result file. Results go to `bench/results/<date>-<config>.json`, and a Python script plots them.

### 8.4 Synthetic load generator
- Takes a recording and **multiplies the traffic**: each copy XORs every ICAO with a different constant (and recomputes CRC/address-parity) and offsets positions. This scales from about 200 to **10,000+ simultaneous aircraft** at real message mixes and up to millions of frames per second.
- It can also inject pathological cases: duplicate bursts, reordered frames, corrupt CRCs, and CPR pairs split across the 10 s window.

### 8.5 Baselines: before versus after
| Comparison | "Before" | "After" |
|---|---|---|
| **System level** | Fall 2025 Python SBS-1 → Postgres → 2 s poll | C++ Beast → multicast → WebSocket |
| **Implementation level** (same C++ code, chosen by a build flag) | `std::mutex` + `std::condition_variable` queues, `std::unordered_map`, `std::string` callsigns, heap allocation per message, unpinned threads | SPSC rings, open-addressing table, POD messages, no allocation, pinned and isolated cores |
| **Ablation** | Turn each optimization off one at a time | This shows which change actually bought the improvement |
| **dump1090 output buffering** | Default flush settings | Minimum flush |

### 8.6 Tail analysis
For every configuration, we explain the p99.9 and max values, not just report them. Tools: `perf record` and `perf stat` (cache misses, branch misses, page faults), `perf sched` and `ftrace` (context switches, IRQ preemption), `/proc/interrupts`, and comparing the timing of outliers against kernel activity. Expected causes, which we will test one at a time: page faults (fixed by `mlockall` and pre-faulting), IRQs on pipeline cores (fixed by IRQ affinity), CPU frequency changes, SMT siblings, timer ticks (`nohz_full`), and hash table probe-length spikes.

---

## 9. Workstream F — Camera Fusion

### 9.1 Setup
- **Camera:** a Raspberry Pi HQ Camera (12 MP, interchangeable lens) or a USB camera with a manual-focus telephoto lens, mounted on a fixed tripod.
- **Site:** pointed at the **approach or departure path of Willard Airport (CMI)**, where aircraft are low, close, and large enough in the image. An airliner at cruise altitude would cover only a few pixels.
- **Compute:** a separate laptop or Jetson-class machine, **not** the pipeline host. That keeps inference off the latency-critical cores.

### 9.2 Pipeline (separate process, subscribes to the multicast feed)
1. **Frame capture with timestamps:** use the sensor timestamp from libcamera / V4L2, which is on `CLOCK_MONOTONIC`, mapped to `CLOCK_REALTIME`. All machines run **chrony** against the same NTP server (or PTP if the NICs support it). The offset between the camera host and the pipeline host is logged continuously.
2. **Extrapolation:** ADS-B positions are seconds old by the time a frame is captured. Each track is propagated to the **exact frame time** with a constant-velocity Kalman filter in local ENU coordinates (a port of the existing [kalman-filter.js](ads/kalman-filter.js) model), producing a predicted position and covariance.
3. **Projection:** geodetic → ECEF → camera-centered ENU → camera frame (extrinsic rotation R, t) → pinhole plus distortion (intrinsics K, d) → pixel (u, v). The position covariance is projected too, giving a **pixel uncertainty ellipse**.
4. **Region-of-interest detection:** the detector runs only inside each track's 3σ ellipse plus a margin, which is the main optimization. Tracks predicted outside the field of view are skipped.
5. **Full-frame scan at low rate (for example 1 Hz):** ROI-only detection **cannot find aircraft that are not broadcasting**, by construction. A periodic full-frame pass finds candidates that are then checked against all tracks.
6. **Detection:** we evaluate two detectors on our footage:
   - *Classical:* sky background subtraction and blob detection. Fast, and good for small dark targets against a uniform sky.
   - *Learned:* a YOLOv8n-class model (via ONNX Runtime). Better against clutter, slower.
7. **Matching:** Hungarian assignment on Mahalanobis distance in pixel space, with χ² gating. A detection that stays unmatched for ≥ N consecutive frames is flagged as an **Unidentified Object** and published on the fusion multicast group, then highlighted in the GUI.

### 9.3 Calibration
- **Intrinsics:** OpenCV checkerboard calibration, done once per lens and focus setting.
- **Extrinsics:** a surveyed camera position (lat/lon/alt), then azimuth, elevation, and roll solved with `solvePnP`, **using matched ADS-B aircraft as the calibration targets**. Their positions are known, so the system refines its own pointing over time. A star field at night gives an independent check.

### 9.4 What we measure
- Clock offset and jitter between hosts, and the projection error (in pixels) it causes.
- Camera capture latency, measured by filming a high-resolution clock or LED on a screen.
- Inference latency with ROI versus full frame, and the speedup.
- **Isolation check:** pipeline latency histograms with fusion running versus not running. A correctly isolated design shows no difference.
- Match rate and false-match rate on labeled footage.

### 9.5 Extension: weather and visibility
- Classify sky conditions (clear, haze, fog, rain) from image features such as contrast, dark-channel prior, and edge density, validated against the **CMI METAR** for the same time.
- Use **"maximum distance at which known ADS-B aircraft are still detected"** as a visibility metric that needs no ground truth. That metric directly supports deciding **when free-space optical (FSO) links are viable**, a concern relevant to microwave and laser HFT networks.

---

## 10. Stretch Goals

We pick these up in this order once the core is done (§14). Each reuses the core rather than starting from scratch.

### 10.1 Market data adapter and limit order book
- **Venue:** the **Kraken v2 WebSocket `book` channel** (public, no auth). It publishes a **CRC32 checksum of the top 10 levels** with each update, which gives us a built-in correctness oracle for our book. Coinbase Advanced Trade `level2` is the fallback.
- **Pipeline:** WebSocket ingest → simdjson parse → normalize to fixed-size `BookDelta` → L2 book (price-indexed arrays around the mid price for the top levels, plus a sorted fallback for outliers) → publish `BookUpdate` over the **same** MoldUDP64-style feed. Snapshot-then-delta reconciliation mirrors §7.3.
- **Measured:** the same per-stage histograms, plus the checksum mismatch rate (target: 0).

### 10.2 Strategy layer and backtester
- An event-driven strategy API (`on_book_update`, `on_trade`, `on_timer`) that runs identically live (paper) and in replay.
- The backtester replays recorded book data with **simulated feed latency and order latency** (configurable distributions) and a simple queue-position fill model. It reports PnL, fill ratio, and **PnL sensitivity to latency**, which is the curve that motivates the whole project.
- **Paper only:** no orders are ever sent to a real exchange.

### 10.3 Hardware demodulator (FPGA)
- Preamble detector (8 µs, 4-pulse pattern), PPM bit slicer, and streaming CRC-24 checker in SystemVerilog. It outputs Beast-format frames, so it plugs into the existing `FrameSource` interface unchanged.
- **Path:** (1) verify in **Verilator** against the raw IQ recordings from §8.1, diffing frames against dump1090 run on the same IQ file. (2) Deploy to a board if one is available, with samples streamed from the host. A direct RF front end is only possible on a board with an ADC.
- **Measured:** frames decoded versus dump1090 at matched signal levels, and the processing latency per sample block.

### 10.4 Multilateration across receiver stations
- **Needs:** ≥ 3 stations (≥ 4 for full 3D without barometric altitude) **several km apart** with overlapping coverage, all publishing their feeds with `beast_ts`.
- **Clock sync is the hard part:** RTL-SDR counters are not synchronized and drift. We use the approach mlat-server uses. **Reference aircraft** that broadcast their own ADS-B positions let us solve for each station's clock offset and drift continuously. GPS-disciplined oscillators are an alternative if hardware is available.
- **Solver:** TDOA equations solved by Gauss-Newton or Levenberg-Marquardt, seeded with the last known position.
- **Validation:** for ADS-B-equipped aircraft, we have ground truth. We report the MLAT error distribution against their self-reported positions. The 12 MHz counter resolution (about 83 ns, or about 25 m) sets a theoretical floor. We expect errors in the hundreds of meters in practice.

---

## 11. Repository Layout

New directories are added next to the existing ones. The Fall 2025 code is kept unchanged as the system-level baseline.

```
group_19_project/
├── README.md                 # Fall 2025 system (unchanged)
├── README_UPDATES.md         # This document
├── PROJECT_REPORT.md         # Fall 2025 report (unchanged)
├── core/                     # NEW — C++20 low-latency pipeline
│   ├── CMakeLists.txt
│   ├── include/adsb/         #   spsc_ring.hpp, clock.hpp, histogram.hpp, beast.hpp, modes.hpp, cpr.hpp, track_table.hpp, feed.hpp
│   ├── src/                  #   ingest/, decode/, state/, publish/, recovery/, main.cpp
│   ├── tools/                #   recorder, replayer, loadgen, feed-cli
│   ├── tests/                #   unit + golden-vector + determinism tests
│   ├── fuzz/                 #   libFuzzer targets (beast parser, decoder)
│   └── bench/                #   Google Benchmark microbenchmarks
├── protocol/                 # NEW — feed_spec.md + schema → C++ / JS / Python struct generators
├── gateway/                  # NEW — Node multicast → WebSocket bridge
├── fusion/                   # NEW — Python camera fusion (capture, calibrate, project, detect, match)
├── bench/results/            # NEW — benchmark JSON + plots (committed)
├── data/recordings/          # NEW — small sample recordings (large ones kept outside git or in Git LFS)
├── ads/                      # Existing web app — modified to consume the WebSocket gateway
├── ads-b-receiver/           # Existing Python streamer — kept as the "before" baseline
└── data-generator/           # Existing mock generator
```

---

## 12. Toolchain and Dependencies

| Area | Choice | Notes |
|---|---|---|
| Language | C++20 | GCC ≥ 12 or Clang ≥ 15. `-O3 -march=native`, with LTO for benchmark builds |
| Build | CMake + Ninja | Presets: `debug-asan`, `release`, `bench`, `naive-baseline` |
| Tests | GoogleTest | Fetched with CMake `FetchContent` |
| Microbenchmarks | Google Benchmark | |
| Fuzzing | libFuzzer + ASan/UBSan | Clang only |
| Hash table reference | Abseil `flat_hash_map` | Benchmark comparison only, never on the hot path |
| JSON (stretch) | simdjson | |
| Fusion | Python 3.11, OpenCV, NumPy, ONNX Runtime | |
| Oracles | readsb, pyModeS | |
| Profiling | `perf`, `ftrace`, `bpftrace` | Linux only |
| CI | GitLab CI | Build, unit tests, golden-vector diff, determinism hash, 60 s fuzz smoke run |

**Platform note:** macOS can be used to edit and to run unit tests, but **all latency numbers come from Linux**. macOS has no hard thread affinity (`thread_policy_set` affinity is only a hint, and is ignored on Apple Silicon), no `isolcpus`, and no `perf`.

---

## 13. Hardware and Test Environments

| Environment | Hardware | Used for |
|---|---|---|
| **Live receiver** | Raspberry Pi 5 (4 cores) + RTL-SDR + 1090 MHz antenna, wired Ethernet | Live capture, recording, live demo |
| **Benchmark host** | x86-64 Linux machine (≥ 6 physical cores, SMT disabled for benchmark runs) | Headline latency and throughput numbers, tail analysis |
| **Camera node** | Pi HQ Camera or USB camera + laptop/Jetson | Camera fusion |
| **Extra receivers** (stretch) | 2–3 more Pi + RTL-SDR kits at separate sites | MLAT |
| **FPGA** (stretch) | TBD (§16) | Hardware demodulator |

Kernel tuning on benchmark hosts is scripted in `core/scripts/tune-host.sh`: `isolcpus`, `nohz_full`, `rcu_nocbs`, IRQ affinity away from pipeline cores, the `performance` governor, turbo/SMT settings recorded, and transparent huge pages set to `madvise`.

---

## 14. Timeline and Milestones

The plan assumes a 14-week semester. Workstreams A–E are sequential and depend on each other. F (camera) runs in parallel from week 4, because it only needs the feed spec and a stub publisher.

| Week | Milestone | Exit criteria |
|---|---|---|
| 1–2 | **M0 — Foundations** | CMake/CI working. SPSC ring, clock, and histogram done with tests. readsb running on the Pi with Beast output. Recorder captures the first datasets. Feed spec v1 drafted. |
| 3–4 | **M1 — Decoder** | All DF17 TCs from §5.1 decoded. Golden vectors pass. Differential diff against readsb at ≥ 99.9% agreement on a peak-hour recording. Fuzzers running in CI. |
| 4–5 | **M2 — Track state** | Open-addressing table with expiry, dedup, and out-of-order handling. Microbenchmarks against `unordered_map` and `flat_hash_map` published. |
| 5–7 | **M3 — Feed** | Publisher, retransmit server, snapshot server, `feed-cli`, and WebSocket gateway done. The web UI runs off the feed with no database in the live path. The gap-recovery test passes under 1% random loss and burst loss. |
| 7–9 | **M4 — Replay and measurement** | Determinism hash passes in CI. Load generator reaches 10k aircraft. Naive baseline build exists. **First full benchmark report** (system-level and implementation-level before/after). |
| 4–11 | **M5 — Camera fusion** (parallel) | Calibrated camera. ROI detection and matching on live footage. Unidentified objects shown in the GUI. Isolation check done. |
| 10–12 | **M6 — Tuning and tail analysis** | Ablation table done. Every p99.9 outlier class explained. Targets in §2 met or the gap explained. |
| 12–13 | **M7 — Stretch** | At least one of: market data/order book, backtester, FPGA in simulation, MLAT. |
| 14 | **M8 — Final** | Final report, demo video, and reproducible benchmark instructions. |

---

## 15. Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Live traffic is too low to stress the pipeline | Benchmarks look the same either way | The synthetic load generator (§8.4) is the primary benchmark input. Live data is used for correctness. |
| The Pi has too few cores for one thread per stage | Pinning has little effect | Benchmark fused versus split stages. Headline numbers come from the x86 host. |
| dump1090/readsb buffering dominates end-to-end latency | Our gains in µs are hidden by ms of upstream buffering | Measure it separately, minimize the flush settings, and report radio → `recv` and `recv` → send as separate numbers. The FPGA stretch goal addresses this. |
| Wi-Fi multicast is unreliable | Spurious gaps | Wired Ethernet only. Loopback for single-host tests. |
| Aircraft too small or too far for the camera | Fusion has nothing to match | Site near the CMI approach, telephoto lens, and the classical small-target detector. |
| Clock offset between hosts corrupts the projection | Mismatches | chrony on all hosts, continuous offset logging, and online extrinsic refinement from matched aircraft. |
| Decoder disagrees with the oracle on edge cases | Blocks M1 | Triage each mismatch class, and document any intended difference (for example, error correction off). |
| MLAT needs remote sites and clock sync | Stretch goal may not finish | Strictly a stretch goal. A fallback demo with two stations shows TDOA alone. |

---

## 16. Open Decisions

These are team and hardware questions to settle in Week 1:

1. **Owners for each workstream.** A natural split: (A+B) decoder, (C+E) state and measurement, (D) feed and gateway, (F) camera fusion.
2. **Benchmark host.** Which x86 Linux machine (personal, lab, or cloud bare-metal) do we get root on for core isolation?
3. **Camera hardware and site.** Which camera and lens, and do we have access to a viewpoint on the CMI approach path?
4. **FPGA board.** Is there a board available through the course, and does it have an ADC or RF front end?
5. **Extra receiver kits** for MLAT: how many, and where can they be hosted?
6. **readsb versus dump1090-fa.** The plan defaults to readsb (more actively maintained, more flexible networking). Confirm after the Week 1 check on the Pi.
7. **Large recordings.** Git LFS or external storage?

---

*IE 421 High-Frequency Trading. Group 19 project extension, building on the Fall 2025 ADS-B Aircraft Tracking System. Advised by Professor David Lariviere.*
