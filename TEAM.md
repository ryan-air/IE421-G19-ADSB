# Team Roles and How We Work

IE 421 Group 19. Read this before you start. It covers who owns what, what is left, and how a change gets into `main`.

- **The plan:** [README_UPDATES.md](README_UPDATES.md) (goals §2, priority tiers §3, timeline §19).
- **Build and test:** [SETUP.md](SETUP.md).

---

## Where we are

About 40% of the plan is done, and everything listed here is on `main` with CI passing.

| Area | What exists | Docs |
|---|---|---|
| Core pipeline (C++) | Mode S / ADS-B decoder, track state engine, `.adsbrec` record and deterministic replay, synthetic traffic generator with ground truth, live ingest from readsb (`adsb-live`) | [core/README.md](core/README.md) |
| Feed | Sequenced multicast feed (MoldUDP64-style) with retransmit and snapshot servers and a subscriber library; wire spec with generated C++/JS/Python; TLA+ model of gap recovery | [protocol/feed_spec.md](protocol/feed_spec.md), [spec/README.md](spec/README.md) |
| Web | WebSocket gateway: pushes the feed to browsers, runs the Fall 2025 map without a database | [gateway/README.md](gateway/README.md) |
| Synthetic IQ | `adsb-iqsynth`: the IQ an SDR would record for synthetic traffic, with Doppler, multipath, the radio horizon, collisions and receiver impairments; SigMF with per-message truth; readsb decodes it | [core/README.md](core/README.md#synthetic-iq-workstream-a-readme_updatesmd-224) |
| Demodulator | `adsb-demod`: IQ samples → CRC-checked Mode S frames; on the generator's files it decodes more messages than readsb (+10% on weak signals) | [core/README.md](core/README.md#demodulator-workstream-a-readme_updatesmd-52) |
| Overload | Loss only at the input boundary and counted; publisher batching under load; tested at 0.5×, 2×, 10× saturation | [core/README.md](core/README.md#overload-semantics-workstream-f-readme_updatesmd-10) |
| Benchmark | Fall 2025 vs. new pipeline on the same traffic: frame → map in the browser 1.1–1.6 s → about 70–90 ms (median, laptop) | [bench/results/legacy-baseline/README.md](bench/results/legacy-baseline/README.md) |

Not started: A/B arbitration, hardware timestamps (on hold), ITCH, and camera fusion. The synthetic IQ generator and the demodulator are done (on synthetic IQ; real recordings and live SDR ingest are next). (The shared-memory ring, the conflated feed, memory-model testing, the io_uring transport and AF_XDP on both sides are done.)

---

## Roles

The plan's tiers apply inside every role: **finish the Must items before starting Should items.** Sizes: S ≈ a few days, M ≈ 1–2 weeks, L ≈ 3+ weeks.

| Role | Owner(s) | Workstreams |
|---|---|---|
| 1. RF and demodulation | **Aniket** (`S-Anikett`) and **Philip** (`philipwestling`) | A, B |
| 2. Decoder, track state, exchange data | **Daniel** (`dsiegal11`) | C, D, J |
| 3. Feed transports and verification | **Ryan** (`ryan-air`) | E, F, H |
| 4. Measurement, isolation, UI, report | *Open until we have the benchmark hardware* | G, I, M7–M8 |

### Role 1: RF and demodulation (Aniket, Philip)

This is the largest open Must item: goal 1 of the plan ("own the wire") depends on it. Plan §5 and §6.

| Item | Tier | Hardware | Size |
|---|---|---|---|
| ~~Synthetic IQ generator with ground truth (extend `adsb-synth`, `core/include/adsb/sim/`)~~ **Done (Ryan):** `adsb-iqsynth`, see `core/README.md` (*Synthetic IQ*) | Must | None | M |
| **Professor's request (plan §22.4):** grow the IQ generator into a scenario generator for testing SDRs: many aircraft at different altitudes and positions, Doppler, multipath, refraction, per-aircraft transmitter frequency offset and drift, colliding replies; IQ in cu8/cs8/cs16/cf32 with SigMF truth; checked against readsb and MATLAB's ADS-B receiver. **Mostly done (Ryan):** all of the above except the MATLAB check (no licence on hand); readsb decodes 98.8% of clean messages at 15–30 dB SNR, timestamps within 0.3 µs (`core/scripts/check-iq.sh`, CI). Still open: Mode A/C replies, aircraft antenna patterns, terrain, the MATLAB check | Must | None | L |
| Transponder frequency-stability study (plan §22.5): per-message frequency estimate, Doppler and receiver error removed, grouped by aircraft and type | Should | RTL-SDR at ≥ 2.4 MS/s, ideally a GPS-disciplined reference | M |
| SIMD demodulator (AVX2 / NEON) tested against the synthetic IQ; target ≤ 10 cycles/sample. **Mostly done (Ryan):** `adsb-demod` (`core/include/adsb/demod/`), scored against the generator's truth and readsb in CI (`core/scripts/check-demod.sh`): 1–20% more messages than readsb on the same files, +10% on weak signals, no false DF17. Still open: SIMD (the magnitude step vectorizes; the preamble check and sequence detector do not), so about 12–23 ns per sample on a laptop, not yet the 10-cycle target | Must | None | L |
| Real IQ recordings; sensitivity ≥ 90% of readsb's on the same recording; USB transfer-size sweep | Must | RTL-SDR (the Fall 2025 Pi setup) | M |
| SDR → IQ ring overload boundary: drop and count (see the overload table in `core/README.md`) | Must | None | S |
| A/B line arbitration: `adsb-synth --lines 2` already simulates two receivers with independent loss | Should | None now; a second RTL-SDR for real data | M |

Start with: `core/include/adsb/sim/synth.hpp` (the generator and its reception model), `core/include/adsb/modes/` (the decoder the demodulator feeds), plan §5.1–5.3.

### Role 2: Decoder, track state, exchange data (Daniel)

Plan §7, §8 and §14.

| Item | Tier | Hardware | Size |
|---|---|---|---|
| Decoder gaps: TC 29/31, Comm-B (BDS), optional single-bit error correction | Must | None | M |
| Find out why our decoder reports about 8% more positions than readsb (`bench/results/legacy-baseline/README.md`, item 7) | Must | None | S |
| ≥ 99.9% agreement with readsb on real traffic; settle the GNSS-height unit question (`core/include/adsb/modes/decoder.hpp`) | Must | Real recordings (OpenSky or our receiver) | S–M |
| Populate `nic`; decide heading and airspeed for feed v1 (`protocol/feed_spec.md` §5) | Must | None | S |
| Nasdaq ITCH 5.0 L3 order book through our MoldUDP64 stack; target ≥ 20 M msg/s per core | Could | None (public sample files) | L |

Start with: `core/README.md` (decoder and engine sections), `core/tests/test_decoder.cpp`, plan §7.

### Role 3: Feed transports and verification (Ryan)

Plan §9.3–9.4, §10.2 and §12.

| Item | Tier | Hardware | Size |
|---|---|---|---|
| ~~Shared-memory broadcast ring (same-host IPC); target p99 < 500 ns~~ **Done:** p99 209 ns at 100 k msg/s on the laptop, see `core/README.md` | Should | None | M |
| ~~Conflated feed: latest state per aircraft at about 10 Hz; the gateway subscribes to it~~ **Done:** `--conflate` on `adsb-live` / `adsb-publish`; point the gateway's `--feed` at it | Should | None | M |
| ~~Memory-model testing of the lock-free rings~~ **Done:** GenMC models of both rings with 20 mutants, `adsb-ring-stress` on x86 and ARM in CI, see `spec/memory-model/README.md` | Should | None | M |
| ~~AF_XDP transport~~ **Done, both sides:** publisher `--transport xdp`; subscriber `--rx xdp` (an XDP program, loaded without libbpf, redirects the feed into an XSK: send → recv p50 1.13 µs vs 1.71 µs through the kernel socket, veth copy mode); tested over a veth pair in CI, see `core/README.md`. Still open: zero-copy numbers on a NIC that supports it (perhaps the lab machine: `bench/lab/run.sh --steps xdp`, see `bench/lab/README.md`) | Should | Zero-copy needs the NIC | L |
| Hardware timestamps and PTP across two hosts | Should | Benchmark host and NIC | L |
| ~~io_uring transport~~ **Done:** `--transport uring` on `adsb-publish` / `adsb-live`, `adsb-tx-bench`, see `core/README.md`; ~~deterministic simulation testing~~ **Done:** `adsb-dst`, per-push and nightly sweeps in CI, see `core/README.md` | Could | None | M each |

Start with: `core/include/adsb/feed/`, `protocol/feed_spec.md`, `spec/`.

### Role 4: Measurement, isolation, UI, report (open)

Most of this waits for the benchmark host. Until then, anyone with spare time can take the items marked "None".

| Item | Tier | Hardware | Size |
|---|---|---|---|
| Set up and tune the benchmark host (`core/scripts/tune-host.sh`); rerun every benchmark there (laptop tail latencies are unreliable). The whole suite is one command: `bench/lab/run.sh` (see `bench/lab/README.md`) | Must | Benchmark host | M |
| Full benchmark report: 10,000-aircraft load, tail analysis (explain every p99.9 outlier class), PGO and ablation table | Must | Benchmark host | L |
| ~~Fold the gateway's map shim into `ads/app.js`, fix the map tiles' API key, update `README.md` and `PROJECT_REPORT.md`~~ **Done:** `ads/app.js` takes the live feed itself (polls the Fall 2025 server as before); Esri dark tiles, no key; new `README.md` for the project (the original is `FALL2025_README.md`); `PROJECT_REPORT.md` marked as the Fall 2025 report | Should | None | S–M |
| Noisy-neighbor isolation matrix (CAT/MBA) | Should | A CPU with RDT | M |
| Minimal camera fusion (plan §13) | Should | Camera and a site | M–L |
| Final report, demo video, reproducible-run instructions | Must | None | M |

### Shared

- **Feed spec:** review `protocol/feed_spec.md`, especially the open questions in §5, then freeze it as v1.
- **Everyone:** run `core/scripts/check-host.sh --json` and share the output.
- **Waiting on Prof. Lariviere** (asked on Discord):
  - the benchmark Linux host with a hardware-timestamping NIC;
  - OpenSky research access;
  - exchange pcaps;
  - whether he has an ADS-B receiver.

  Asked again on 2026-11-19 together with the reply to his feedback (plan §22). Hardware and data list: plan §22.6.

  Roles 3 and 4 depend most on these.

---

## How to contribute

**Nobody pushes to `main` directly.** Every change goes through a pull request, which **all three other teammates review**. After everyone has approved and CI is green, **a reviewer (not the author) merges it.**

### 1. Start from an up-to-date `main`

```bash
git checkout main
git pull origin main
git checkout -b demod/synthetic-iq          # role/topic, short and descriptive
```

Branch names: `<area>/<topic>`, for example `demod/synthetic-iq`, `decoder/tc31`, `feed/shm-ring`, `report/tail-analysis`.

### 2. Make the change, with tests

- **Code changes need tests:**
  - decoder changes need test vectors;
  - feed or engine changes need unit tests, plus an end-to-end check where it applies.
- **Run the checks** from [SETUP.md](SETUP.md) before pushing:
  - the C++ tests in at least the `debug` and `asan` presets;
  - the gateway tests if you touched `gateway/`;
  - `python3 protocol/gen.py --check` if you touched the protocol.
- **CI builds with GCC and Clang at `-Werror`.** GCC catches warnings Clang does not, so if you develop on a Mac, expect CI to flag a few.
- **Feed format changes:** edit `protocol/schema/feed_v0.json` only, then run `python3 protocol/gen.py`. Never edit the generated files.
- **Never commit** secrets, `.env` files, large recordings, or build output.

### 3. Commit

Small, focused commits with a message that says what changed and why:

```bash
git add -p                                   # review each hunk you stage
git commit -m "Add a synthetic IQ generator with ground truth" -m "Generates 2 MS/s IQ for Mode S frames from adsb-synth traffic ..."
```

The first line is a short summary in the imperative ("Add …", "Fix …"). After a blank line, explain what changed and why: what you measured, and what you checked.

### 4. Push and open a pull request

```bash
git push -u origin demod/synthetic-iq
gh pr create --base main --fill             # or open it on github.com
```

In the PR description, include:
- what changed and why;
- how you tested it (commands, results, numbers with their caveats);
- anything reviewers should look at closely, and anything not done yet.

Request a review from all three other teammates.

### 5. Review

- **Everyone else reviews every PR,** within about two days if possible. Read the diff, run it locally if it touches something you know, and either approve or leave specific comments.
- **The author answers every comment,** pushes fixes to the same branch, and re-requests review.
- **Approve when:**
  - the change does what the description says;
  - it has tests;
  - CI is green;
  - you would be comfortable maintaining it.

### 6. Merge

- **Who merges:** once **all three reviewers have approved** and **CI is green**, one of the reviewers merges, never the author.
- **How:** use **"Squash and merge"** for most PRs, so `main` gets one clean commit per change.
- **After merging:** delete the branch.

If `main` moved while your PR was open, update your branch before the merge:

```bash
git checkout demod/synthetic-iq
git fetch origin
git rebase origin/main                       # fix any conflicts, rerun the tests
git push --force-with-lease
```

### Rules of thumb

- **Keep PRs small.** One feature or fix per PR is much easier to review than a month of work at once.
- **Ask before taking work in someone else's role.** If you are blocked on another role, say so in the PR or on Discord.
- **Report numbers honestly:** say where they were measured (laptop or benchmark host) and what they leave out.
