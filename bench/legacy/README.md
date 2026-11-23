# Before/After Benchmark: Fall 2025 vs. the New Pipeline

Measures the Fall 2025 system as delivered (the "before") and the new pipeline end to end (the "after"), on the same synthetic traffic, frame by frame against ground truth (README_UPDATES.md §11.5). Results: [`bench/results/legacy-baseline/report.md`](../results/legacy-baseline/report.md).

```bash
bench/legacy/run.sh                                   # full sweep: 50, 200, 1000, 3000 aircraft, 120 s each, both systems (~25 min)
SCENARIOS="6000" bench/legacy/run.sh                   # heavier load: needs ~2 GB free disk; check feeder lateness
SCENARIOS="50 200" DURATION=60 bench/legacy/run.sh    # quick run
```

Requires Docker and about 2 GB of free disk. Results go to `bench/results/legacy-baseline/`. Each run's raw logs are deleted once analyzed; set `KEEP_RAW=1` to keep them in `bench/legacy/out/` (not committed).

## What runs

```
adsb-synth ──.adsbrec──▶ feeder.py ──Beast──▶ readsb ──SBS-1 :30003──▶ adsb_streamer_optimized.py ──▶ Postgres
 (traffic +            (real-time pace)     (dump1090                   (Fall 2025, production flags:      │
  ground truth)                              stand-in)                   --batch-size 100 --flush 1.0)      ▼
                                                                                         ads/server.js  /api/aircraft
                                                                                         (Fall 2025)    (the map polls every 2 s)
```

| Piece | Stand-in for | Notes |
|---|---|---|
| `adsb-synth` | Real aircraft | Synthetic traffic around CMI with known ground truth (`core/`) |
| `feeder.py` | RTL-SDR | Sends each frame to readsb's Beast input at the moment it would have arrived |
| readsb v3.16.21 | dump1090 on the Pi | Same software family, default output buffering. Pinned tag, built from source. |
| Postgres 16 | Supabase | Local, SSL on: the Fall 2025 server always connects with SSL |
| Streamer, web server | — | Run exactly as delivered: `ads-b-receiver/` and `ads/` from the git tag `fall2025-final` |

**The after pass.** For each fleet size, the same recording is then fed again, in real time, to a fresh readsb, and the new pipeline runs alone:

```
feeder.py ──Beast──▶ readsb ──Beast :30005──▶ adsb-live ──multicast feed──▶ adsb-gateway ──WebSocket──▶ probe.py ws
                                              (decode, track state,          (gap recovery,       (what the map receives)
                                               publisher, retransmit)         conflated fan-out)  + /api/aircraft from memory
```

readsb is the same build and configuration in both passes; it stays in the "after" path as the receiver stand-in until the project's own demodulator (Workstream A) replaces it. The gateway runs on the image's Node.js, the same one that runs the Fall 2025 web server.

**The Fall 2025 code comes from the git tag `fall2025-final`, not from the working tree.** The tag points at the last commit of the Fall 2025 GitLab repository (`7b52633`, 2025-12-21). `run.sh` extracts `ads/` and `ads-b-receiver/` from it with `git archive` into `bench/legacy/fall2025/` (gitignored, recreated every run), and the image, the database schema and the processes all use that copy. The working-tree `ads/` and `ads-b-receiver/` may be changed by later work, and the "before" numbers stay those of the delivered system. `environment.json` records the tag and commit measured. Override with `FALL2025_REF=<ref>` only to measure something else on purpose.

The Fall 2025 code is not modified. Two database fixes are applied at startup, both as separate init scripts:
- [`supabase_roles.sql`](supabase_roles.sql) creates the Supabase roles (`authenticated`, ...) that `schema.sql` grants permissions to.
- [`position_history.sql`](position_history.sql) creates `aircraft_position_history`. The streamer writes this table and the server reads it, but `ads-b-receiver/schema.sql` does not create it. The definition is taken from PROJECT_REPORT.md. **This is a bug in the committed schema:** a fresh deployment from the repository fails on every batch insert.

## What is measured

All timestamps are taken inside one container, so every probe shares one clock.

| Probe | Records |
|---|---|
| `probe.py sbs` | Every SBS-1 line readsb emits, with its arrival time |
| `probe.py db` | Polls `aircraft_real` every 100 ms and logs each change of an aircraft's position |
| `probe.py api` | Polls `GET /api/aircraft` (what the map uses) every 100 ms: changes, plus a full snapshot every second |
| `probe.py cpu` | CPU time of readsb, the streamer, and the web server (after pass: readsb, `adsb-live`, the gateway), from `/proc` |
| `probe.py ws` | After pass: connects to the gateway's WebSocket as a browser does; logs every position change with the arrival time of its frame, and a full snapshot of the table every second |

`analyze.py` matches every position shown at each stage to the **transmitted frame it came from**. Per aircraft, the source is the ground-truth position nearest the shown one: frames are at least 0.4 s apart (about 100 m at airliner speed) and CPR error is about 5 m, so the match is unambiguous. Anything farther than 50 m is counted as unmatched, not guessed. From that it computes:
- **Latency** from frame received to visible, at each stage. Before: readsb output, database, API, and the browser (API plus the map's uniformly distributed 0–2 s poll wait). After: WebSocket arrival, the same API probe against the gateway, and the browser (WebSocket arrival plus the new map's render wait: uniform 0–100 ms plus one animation frame). Inside the new pipeline, `adsb-live` reports receive → track state and the gateway reports publish → book and publish → WebSocket write.
- **Coverage:** the share of transmitted positions that ever reach the database (before) or the WebSocket (after). The generator models reception loss, so neither reaches 100%.
- **The map view:** how old the shown position is, and how far it is from where the aircraft actually is at that moment. The browser variant holds each snapshot until the next 2 s poll.
- **Keeping up:** median database latency early against late in the run (growth means a backlog), CPU per process, and how late the feeder delivered frames.
- **Decoder comparison:** readsb's positions, altitudes, callsigns, and squawks against the new C++ decoder's, both checked against ground truth. readsb's positions are its SBS `MSG,3` (airborne) and `MSG,2` (surface) lines; the new decoder emits both kinds.

### Position by position: `compare-positions.sh`

`bench/legacy/compare-positions.sh [AIRCRAFT] [SECONDS] [SEED]` (default 300, 90, 5; about a minute plus the duration) feeds one synthetic recording to readsb in this image and replays it through `adsb-replay --out`. `compare_positions.py` then matches every position one to one, by aircraft and coordinates (within 2e-5°, the SBS rounding), and explains whatever is left on each side. Results go to `bench/legacy/out/positions-nN/summary.txt`. At 1,000 aircraft for 60 s (seed 7):

| | Positions |
|---|---|
| New decoder | 84,495 |
| readsb `MSG,3` (airborne) | 77,803: the new decoder looks 8.6% ahead, the old finding |
| readsb `MSG,2` (surface) | 6,994: with them, the new decoder has 0.36% **fewer** |
| Matched one to one | 84,450 |
| Only the new decoder | 45, all surface positions within 0.9 m of the truth, 38 of them among a ground track's first three (readsb prints a surface track only after a few positions) |
| Only readsb | 347: 242 next to a corrupted frame of that aircraft (readsb repairs single-bit errors by default, the new decoder rejects them); 29 where a repaired frame completed readsb's even/odd CPR pair; 76 an unchanged position printed again (the track engine sends only changes); 0 unexplained |

So the two decoders agree position for position; the difference is readsb's single-bit repair, which the new decoder leaves out by default. `FIX=1 bench/legacy/compare-positions.sh 1000 60 7` runs the new decoder with `--fix` (single-bit repair of DF17/18; results in `positions-n1000-fix/`): it then has 0.9994 of readsb's positions; what remains is mostly readsb printing unchanged positions again, and frames with a wrong format bit, which are never repaired.

Finally the recording is replayed through the new pipeline's core alone (`adsb-replay --pipeline --speed 1 --busy-wait`: no network, no readsb), for context.

## Caveats

- **Lower bound on production latency:** Postgres is local. In production every batch crossed the internet to Supabase.
- **Browser columns:** estimates, as described above. Rendering itself is not included for either system, nor the Fall 2025 map's 2 s marker animation (the new map moves markers immediately).
- **readsb buffering is in both paths:** with default settings it batches its output, which accounts for most of the new pipeline's WebSocket latency. Removing it is Workstream A's job, not this benchmark's.
- **Probe resolution:** about 100 ms, which adds about 50 ms on average to database and API latencies (in both passes). The WebSocket probe has no polling delay.
- **Machine dependence:** throughput limits and CPU use depend on the machine. Rerun on the benchmark host for the final report. The latency structure (batch flush, rate limit, polling) is machine-independent.
- **One system at a time:** the two passes run sequentially on the same input, so neither competes with the other for CPU. Background load on the host still affects both.
- **CPU:** `adsb-live`'s track-state thread spins instead of sleeping, so it uses one core even at light load (a latency-for-CPU trade, as in feed handlers).
- **Load-generator limit:** at very high load the Python feeder itself may fall behind, so check "feeder lateness" in the report. Where it is large, that run measures the feeder, not the pipeline.
