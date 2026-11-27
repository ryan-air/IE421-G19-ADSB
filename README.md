# ADS-B as a Market Data Feed

IE 421 (High-Frequency Trading), University of Illinois Urbana-Champaign, Group 19.

Aircraft broadcast their position, altitude and speed on 1090 MHz (ADS-B) many times a second. This project treats that stream like exchange market data. It decodes frames into a live track table (the "book") in well under a microsecond, publishes every change on a sequenced multicast feed with gap recovery, and pushes the book to browsers. It uses the techniques of a trading system: lock-free queues, no allocation on the hot path, deterministic replay, kernel-bypass transports, and model checking of the protocols. The same infrastructure will later carry Nasdaq ITCH 5.0.

It extends the Fall 2025 ADS-B tracker, in which a Python streamer wrote to Postgres and a web map polled it every 2 seconds. That system is documented in [FALL2025_README.md](FALL2025_README.md) and [PROJECT_REPORT.md](PROJECT_REPORT.md), and is kept as the benchmark's baseline.

## How it fits together

```
receiver (RTL-SDR) → readsb ──Beast TCP──▶ adsb-live: decode + track state (C++)
                                              │  sequenced feed: kernel UDP multicast, io_uring, AF_XDP, or shared memory
                                              │  + retransmit (UDP) and snapshot (TCP) servers for gap recovery
                                              ▼
                       subscribers (C++ adsb-feed-cli, shared-memory readers)
                       adsb-gateway (Node) ──WebSocket, conflated per client──▶ map in the browser (ads/)
```

Recorded or synthetic traffic (`.adsbrec`) can stand in for the receiver: `adsb-publish` replays a recording onto the same feed, deterministically. `adsb-iqsynth` goes one step earlier: it writes the raw IQ samples an SDR would record for the same synthetic traffic (SigMF, with per-message truth), the test input for our demodulator `adsb-demod` and for other receivers ([core/README.md](core/README.md#synthetic-iq-workstream-a-readme_updatesmd-224)).

## Results so far

These were measured on a laptop. They are medians, because laptop tails are unreliable. The final numbers come from the benchmark host (`bench/lab/run.sh`).

| | Fall 2025 system | This project |
|---|---|---|
| Frame → aircraft on the map in the browser (estimated) | 1.1–1.6 s | 69–88 ms |
| Frame → WebSocket message at the browser | — | 4.8–30 ms (mostly readsb's output buffer, which flushes at 1,280 bytes or 50 ms) |
| Positions that reach the map | 17% | 76–78% |
| Map position error against ground truth | 461–540 m | 66–69 m |
| Frame → track state updated | — | 0.4–0.8 µs |
| Publish → subscriber's book updated (loopback UDP) | — | p50 49 µs |
| Shared-memory ring, write → read | — | p50 125 ns, p99 209 ns |
| AF_XDP receive instead of the kernel socket | — | send → receive p50 1.71 → 1.13 µs |
| Synthetic 1090 MHz IQ (Doppler, multipath, collisions, receiver impairments) decoded by readsb | — | 98.8% of clean messages at 15–30 dB SNR; timestamps within 0.3 µs of the truth |
| Our demodulator on the same IQ files, against readsb | — | 1–20% more messages than readsb (+10% on weak signals), no false DF17; starts within 0.06 µs of the truth; about 70× real time on one core at 2.4 MS/s with SIMD (readsb is about 1.1× faster) |
| IQ sample → track state, through our demodulator (`adsb-live --iq`, 6.8 ms blocks) | — | p50 5.2 ms, p99 9.2 ms (mostly waiting for the rest of the block; 1.2 ms with 1.7 ms blocks) |
| Two receivers merged (A/B arbitration, like an exchange's A and B feeds; synthetic IQ, two receivers with independent noise) | — | 16% more frames than the better receiver alone, every copy matched; about 25 ns per frame |
| Transponder carrier frequency per message, from IQ (frequency-stability study; synthetic IQ with known offsets, Doppler and receiver error) | — | per message p50 23 Hz; each aircraft's offset within 6 Hz (p50) and drift within 0.08 Hz/s; Doppler from the decoded track within 0.4 Hz |

The comparison runs both systems on the same traffic through readsb, at 50–3,000 aircraft. Details are in [bench/results/legacy-baseline/](bench/results/legacy-baseline/README.md) and [core/README.md](core/README.md).

## Quick start

You need CMake ≥ 3.24, a C++20 compiler (GCC or Clang; Apple clang 15+ on macOS), Python 3 and Node 22 (details for each OS in [SETUP.md](SETUP.md)). Build and test:

```bash
cd core && cmake --preset release && cmake --build --preset release && ctest --preset release && cd ..
```

Then watch it run. The commands below generate 15 minutes of synthetic traffic over Chicago, start the gateway, and publish the traffic in real time:

```bash
core/build/release/tools/adsb-synth -o /tmp/demo.adsbrec --aircraft 150 --duration 900 --receiver 41.88,-87.63
```

```bash
node gateway/bin/adsb-gateway.mjs --feed 127.0.0.1:41900 &
```

```bash
core/build/release/tools/adsb-publish /tmp/demo.adsbrec --feed 127.0.0.1:41900 --bind 127.0.0.1 --speed 1 --busy-wait
```

Then open http://127.0.0.1:8080/ for the live map. The gateway's counters are at http://127.0.0.1:8080/gateway/stats.

To track real aircraft, run readsb with an RTL-SDR receiver and point `adsb-live` at its Beast output (see [core/README.md](core/README.md)).

[SETUP.md](SETUP.md) covers everything else: sanitizer builds, fuzzing, the determinism check, the protocol code generator, the TLA+ model, and the baseline benchmark.

## Repository

| Path | What it is |
|---|---|
| [`core/`](core/README.md) | The C++ pipeline: decoder, track state, recording and replay, synthetic traffic, the feed (publisher, recovery, subscribers; kernel UDP, io_uring, AF_XDP, shared memory), the conflated feed, overload handling, deterministic simulation testing, tools and tests |
| [`protocol/`](protocol/feed_spec.md) | The feed's wire format: one JSON schema, generated C++/JavaScript/Python code, golden packets |
| [`spec/`](spec/README.md) | TLA+ model of gap recovery; GenMC models of the lock-free rings |
| [`gateway/`](gateway/README.md) | WebSocket gateway (Node, no dependencies): feed subscriber, per-client conflation and flow control, serves the map |
| `ads/` | The web map (Leaflet) and the 3D globe (Cesium). Both take the live feed from the gateway, or poll the Fall 2025 server |
| [`bench/`](bench/legacy/README.md) | Fall 2025 vs. new pipeline benchmark (Docker), live-ingest test against readsb, the benchmark-host suite ([`bench/lab/`](bench/lab/README.md)), results |
| `ads-b-receiver/`, `data-generator/` | The Fall 2025 streamer and test data generator |

## Documents

- [README_UPDATES.md](README_UPDATES.md): the plan. It covers goals and targets, design, what is built, and the timeline.
- [TEAM.md](TEAM.md): roles, what is left, and how a change gets into `main`.
- [SETUP.md](SETUP.md): build, test and tooling.
- [protocol/feed_spec.md](protocol/feed_spec.md): the feed protocol.
- [REPRODUCING.md](REPRODUCING.md): every reported result, with the commands that reproduce it and which numbers still need the benchmark host.
- [MATLAB comparison](bench/matlab/README.md): optional DF17 cross-check and receiver diagnostics on synthetic IQ.
- [REPORT.md](REPORT.md): the final report's skeleton, with the results so far and placeholders for the pending ones.

## Team

| Role | Who |
|---|---|
| RF and demodulation | Aniket, Philip |
| Decoder, track state, exchange data | Daniel |
| Feed transports and verification | Ryan |
| Measurement, isolation, UI, report | open |

Advisor: Prof. David Lariviere. Changes reach `main` only through pull requests that every other teammate reviews (see [TEAM.md](TEAM.md)).
