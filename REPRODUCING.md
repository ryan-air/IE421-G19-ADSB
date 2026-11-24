# Reproducing the Results

Every experiment and result this project reports, with the commands that reproduce it. The commands come from the component docs and from CI (`.github/workflows/core.yml`, `.github/workflows/dst-nightly.yml`). CI is the ground truth: if a command here and a CI step disagree, the CI step is what actually runs on every push.

**Where the numbers come from.** Every number in the docs so far was measured on a laptop (Apple M2, macOS, or Linux in Docker Desktop's VM), usually under background load. Medians are fairly stable there; tails (p99 and above) are not. Numbers marked **Benchmark host** below must be rerun on the Linux benchmark host before they go into the final report, normally through `bench/lab/run.sh` ([last section](#benchmark-host-suite)). CI runners are shared VMs: CI checks correctness, and its timings mean nothing.

Paths are relative to the repository root unless a command starts with `cd core`. "CI limit" is the job's `timeout-minutes`, an upper bound on runtime.

## Contents

1. [Prerequisites](#prerequisites)
2. [Build and test suites](#build-and-test-suites)
3. [Determinism](#determinism)
4. [Protocol generation](#protocol-generation)
5. [Formal models: TLA+ and GenMC](#formal-models-tla-and-genmc)
6. [Feed and transports](#feed-and-transports)
7. [Overload](#overload)
8. [Deterministic simulation](#deterministic-simulation)
9. [Gateway and the browser map](#gateway-and-the-browser-map)
10. [Before/after benchmark against the Fall 2025 system](#beforeafter-benchmark-against-the-fall-2025-system)
11. [Live ingest against readsb](#live-ingest-against-readsb)
12. [Synthetic IQ, demodulator, IQ live, A/B arbitration, carrier frequency](#synthetic-iq-demodulator-iq-live-ab-arbitration-carrier-frequency)
13. [Microbenchmarks](#microbenchmarks)
14. [Benchmark-host suite](#benchmark-host-suite)
15. [What needs hardware](#what-needs-hardware)

---

## Prerequisites

From [SETUP.md](SETUP.md) §2:

| Need | For |
|---|---|
| CMake ≥ 3.24, a C++20 compiler (GCC or Clang; Apple clang 15+ on macOS), git; Ninja optional | Everything in `core/` |
| Internet access on the first configure | GoogleTest and Google Benchmark are downloaded |
| Python 3 | `protocol/gen.py`, the scoring scripts, the benchmark analysis |
| Node 22 | `gateway/`, the JavaScript protocol test |
| Java 11+ and curl | TLC (`spec/run-tlc.sh`) |
| Docker, about 2 GB free disk | Before/after benchmark, live ingest, Linux checks from a Mac, GenMC on a Mac |
| Linux with `liburing-dev`, `pkg-config` | io_uring transport |
| Linux, root or file capabilities, a veth pair or two NIC ports | AF_XDP |
| readsb v3.16.21 built with `RTLSDR=no` | Scoring the IQ generator and comparing the demodulator (optional for `check-demod.sh`) |

On Ubuntu 24.04: `sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git clang liburing-dev pkg-config`.

---

## Build and test suites

### Presets

From `core/` (SETUP.md §3–4, core/README.md *Build and test*):

```bash
cd core
cmake --preset debug && cmake --build --preset debug -j && ctest --preset debug
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan -j && ctest --preset tsan
cmake --preset release && cmake --build --preset release -j && ctest --preset release
cmake --preset naive-baseline && cmake --build --preset naive-baseline -j && ctest --preset naive-baseline
```

| Preset | What it is |
|---|---|
| `debug` | Debug build, tests, tools |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer (required for anything with threads or atomics) |
| `release` | Optimized build, tests, tools. Most commands below use `core/build/release/tools/` |
| `naive-baseline` | Mutex queues and `std::unordered_map` instead of the SPSC ring and the flat table; the same tests must pass |
| `bench` | `-O3`, native CPU, LTO, Google Benchmark |
| `fuzz` / `fuzz-standalone` | libFuzzer (Linux, LLVM clang) / portable driver (macOS) |

Expected: `100% tests passed`. Each preset builds into `core/build/<preset>/`. Add `-DADSB_WERROR=ON` to the configure step to build like CI. On Linux, sanitizer runs may need `sudo sysctl -w vm.mmap_rnd_bits=28` (SETUP.md §12).

**CI:** job `test` runs `cmake --preset P -G Ninja -DADSB_WERROR=ON`, build, `ctest --preset P` for GCC 14 and Clang 18 × `debug`, `asan`, `tsan`, `release`, plus GCC `naive-baseline`.

Note: `core/build/` is shared by every branch. After switching branches, rebuild all tools before running the integration scripts, or they mix binaries from two versions.

### Linux GCC/Clang check from a Mac

macOS presets do not use `-Werror`, and GCC flags warnings Clang does not. From the repository root, with Docker running (SETUP.md §10 uses `g++`; this is the same with GCC 14, as in CI):

```bash
docker run --rm -v "$PWD":/src:ro ubuntu:24.04 bash -c "apt-get update -qq && apt-get install -y -qq cmake ninja-build g++-14 ca-certificates >/dev/null && CXX=g++-14 cmake -S /src/core -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DADSB_WERROR=ON && cmake --build /tmp/b && ctest --test-dir /tmp/b"
```

Mount the whole repository, not only `core/`: tests read `protocol/golden/`. For Clang, install `clang-18` and set `CXX=clang++-18`. CI's `benchmarks build` and `gcc / naive-baseline` jobs also compile `core/bench/`, so add `-DADSB_BUILD_BENCH=ON` to catch warnings there. A container is fine for correctness, not for latency.

### Fuzzing

From `core/` (SETUP.md §7):

```bash
cmake --preset fuzz-standalone && cmake --build --preset fuzz-standalone -j        # macOS, any compiler
./build/fuzz-standalone/fuzz/fuzz_decoder -runs=1000000 -max_len=15 fuzz/corpus/decoder
./build/fuzz-standalone/fuzz/fuzz_beast -runs=300000 fuzz/corpus/beast

cmake --preset fuzz && cmake --build --preset fuzz -j                              # Linux, LLVM clang
./build/fuzz/fuzz/fuzz_decoder -max_total_time=300 fuzz/corpus/decoder
```

Targets: `beast`, `decoder`, `packet`, `feed`, `demod`, `carrier`, each with a corpus in `core/fuzz/corpus/`. **CI:** job `fuzz` runs each target for 60 s with clang 18 (`fuzz_$target -max_total_time=60`); crashes are uploaded as `crash-*`.

---

## Determinism

The replay contract (README_UPDATES.md §11.2): one recording and one build give byte-identical output at any speed, inline or two-thread, in the optimized and the naive build, and with GCC and Clang (possible because every target builds with `-ffp-contract=off`).

```bash
cd core
cmake --preset release && cmake --build --preset release -j
cmake --preset naive-baseline && cmake --build --preset naive-baseline -j
./scripts/check-determinism.sh build/release build/naive-baseline
```

Exits nonzero on any mismatch. **CI:** job `determinism` also compares a Clang 18 build: `./scripts/check-determinism.sh build/release build/naive-baseline build/clang-release`.

By hand (SETUP.md §5):

```bash
./build/release/tools/adsb-synth -o /tmp/traffic.adsbrec --aircraft 200 --duration 120 --lines 2
./build/release/tools/adsb-replay /tmp/traffic.adsbrec                          # counters + SHA-256 of the engine output
./build/release/tools/adsb-replay /tmp/traffic.adsbrec --pipeline --speed 20    # two threads, latency percentiles: same hash
./build/naive-baseline/tools/adsb-replay /tmp/traffic.adsbrec --pipeline --speed 20
```

| Result | Source | Where measured |
|---|---|---|
| Pipeline latency at 65k frames/s, frame in → state updated: optimized p50 0.38 µs / p99 1.8 µs / p99.9 21 µs; naive p50 1.2 µs / p99 29 µs / p99.9 830 µs | core/README.md *Indicative numbers* | Laptop. **Benchmark host** (pinned: `adsb-replay ... --cpus 2,3`) |

---

## Protocol generation

The feed's wire format is defined once in `protocol/schema/feed_v1.json`; `protocol/gen.py` generates the C++, JavaScript and Python code. From the repository root (SETUP.md §6):

```bash
python3 protocol/gen.py --check                                     # generated files are up to date
python3 -m unittest discover -s protocol/tests                       # Python golden-packet tests
node protocol/tests/test_feed.mjs                                    # JavaScript golden-packet tests
```

After editing the schema, run `python3 protocol/gen.py` (never edit the generated files). **CI:** job `protocol`, the same three commands.

---

## Formal models: TLA+ and GenMC

### TLA+ model of gap recovery

```bash
bash spec/run-tlc.sh
```

Needs Java 11+ and curl. Downloads `tla2tools.jar` v1.7.4 into `spec/.tools/` (SHA-256 checked), runs three configs and compares each with its expected outcome; logs in `spec/out/*.out`.

| Config | Expected | Measured (spec/README.md, macOS, 8 workers) |
|---|---|---|
| `MC.cfg` | exit 0: every invariant and liveness property holds | 1,457,453 states, 211,522 distinct, 23 s |
| `MC_NoSnapshot.cfg` | exit 13: liveness violated (no snapshot fallback) | 3 s |
| `MC_Reach.cfg` | exit 12: `NeverSnapshot` violated (the snapshot path is reachable) | < 1 s |

About 30 s in total. **CI:** job `tlc` (Java 21, CI limit 15 min).

### GenMC model checking of the lock-free rings

GenMC needs LLVM 19 on Linux. From a Mac, in Docker (spec/memory-model/README.md):

```bash
docker run --rm -v "$PWD":/src:ro ubuntu:24.04 bash -c \
  "cp -r /src/spec /tmp/spec && G=\$(bash /tmp/spec/memory-model/build-genmc.sh) && GENMC=\$G bash /tmp/spec/memory-model/run.sh"
```

On Ubuntu 24.04 directly: `GENMC=$(bash spec/memory-model/build-genmc.sh) bash spec/memory-model/run.sh`. Building GenMC takes about 10 minutes (and a few GB of memory: do not run it next to a large build); `run.sh` takes 15–30 s. Expected: the 6 correct models verify and all 20 mutants are caught (the run fails otherwise). **CI:** job `memory-model` (GenMC v0.20.0 build cached, CI limit 30 min).

### Ring stress on real hardware

```bash
cd core
cmake --preset release && cmake --build --preset release -j
./build/release/tools/adsb-ring-stress --seconds 3600 --seed 7
```

Run it on ARM if possible. **CI:** job `ring-stress`, 60 s on `ubuntu-24.04` and `ubuntu-24.04-arm` (`adsb-ring-stress --seconds 60 --seed <run number>`).

---

## Feed and transports

All from `core/` after a release build. Each script takes a build directory and prints `PASS`/`FAIL` per check; it exits nonzero if any check fails. `KEEP=1` keeps a script's work directory.

### Kernel UDP feed: loss, snapshots, late join, restart

```bash
FEED_IFACE=127.0.0.1 scripts/check-feed.sh build/release
```

15 checks: three subscribers (no extra loss; bursts of 4,000 lost datagrams that outrun a 2,048-message ring and force snapshots; a 1.5 s late joiner) and a publisher restart. Every subscriber's book must equal the publisher's, and the publisher's must equal a plain replay's. **CI:** job `feed` (CI limit 15 min).

By hand (core/README.md *Using the tools*): start a subscriber, then the publisher; both print the same `book digest` line.

```bash
./build/release/tools/adsb-feed-cli --feed 239.19.0.1:41900 --interface 127.0.0.1 --until-end &
./build/release/tools/adsb-publish /tmp/traffic.adsbrec --feed 239.19.0.1:41900 --interface 127.0.0.1 --speed 10 --drop-rate 0.01
```

| Result | Source | Where measured |
|---|---|---|
| Publish → applied in a subscriber, UDP loopback unicast, 11k msg/s, no loss: p50 49 µs, p99 79 µs (macOS loopback multicast has ~26 ms p99 stalls) | core/README.md *Indicative numbers* | Laptop. **Benchmark host** |

### Shared-memory ring

```bash
scripts/check-shm.sh build/release
./build/release/tools/adsb-shm-bench --rate 100000 --cpus 2,3        # ring alone
./build/bench/bench/adsb_bench --benchmark_filter=Shm                 # needs the bench preset (see Microbenchmarks)
```

`check-shm.sh`: 13 checks (a subscriber that keeps up, one lapped and resynced by snapshot, a late joiner, a publisher restart). End to end: publish with `--shm NAME` on `adsb-publish` or `adsb-live`, subscribe with `adsb-shm-cli --name NAME`. **CI:** job `shm`.

| Result | Source | Where measured |
|---|---|---|
| Ring alone, write → read on another thread, 100k msg/s: p50 125 ns, p99 209 ns (target p99 < 500 ns) | core/README.md | Laptop (24 MHz timer, no isolation). **Benchmark host** |
| End to end `adsb-publish --shm` → `adsb-shm-cli`, 300 aircraft at 10×: p50 125 ns, p99 3.2–3.7 µs | core/README.md | Laptop. **Benchmark host** |

### Conflated feed

```bash
FEED_IFACE=127.0.0.1 scripts/check-conflate.sh build/release
```

23 checks, from `adsb-publish` and `adsb-live` (fed by `adsb-beast-serve`): subscribers that keep up, lose bursts, or join late end with the main feed's state (by state digest); deletes cross the conflated feed; the conflated feed is the same at 10× and full speed and sends fewer updates. **CI:** job `conflate`. Enable on the tools with `--conflate GROUP:PORT` (and `--conflate-hz`).

### io_uring (Linux)

```bash
cmake -S . -B build/uring -G Ninja -DCMAKE_BUILD_TYPE=Release -DADSB_WERROR=ON -DADSB_IO_URING=ON
cmake --build build/uring
ctest --test-dir build/uring -R "Uring|^FeedTx\." --output-on-failure
FEED_IFACE=127.0.0.1 FEED_TRANSPORT=uring scripts/check-feed.sh build/uring
FEED_IFACE=127.0.0.1 FEED_TRANSPORT=uring-nosqpoll scripts/check-feed.sh build/uring
FEED_TRANSPORT=uring scripts/check-overload.sh build/uring
build/uring/tools/adsb-tx-bench --seconds 2 --rounds 2
```

`-DADSB_IO_URING=ON` fails the build if liburing is missing (instead of compiling a stub and skipping the tests). In Docker, io_uring needs `--security-opt seccomp=unconfined`. **CI:** job `uring` (the same steps; it also checks no io_uring test was skipped).

| Result (1,200-byte datagrams, 100k/s, loopback) | Source | Where measured |
|---|---|---|
| `send()` p50: kernel 1,459 ns, io_uring SQPOLL 208 ns, no SQPOLL 1,625 ns; send → recv p50 1.5 / 1.7 / 1.6 µs | core/README.md | Linux 6.10 in Docker on the laptop, load 10–13, medians only. **Benchmark host** (`bench/lab/run.sh`, step `transports`) |

### AF_XDP (Linux, both sides)

Over a veth pair (core/README.md; needs `CAP_NET_ADMIN`, `CAP_NET_RAW`, and `CAP_BPF` for the receiving side; in Docker add `--cap-add NET_ADMIN --sysctl net.ipv4.conf.all.accept_local=1 --sysctl net.ipv4.conf.all.rp_filter=0`):

```bash
cmake -S . -B build/xdp -G Ninja -DCMAKE_BUILD_TYPE=Release -DADSB_WERROR=ON -DADSB_AF_XDP=ON && cmake --build build/xdp
ip link add veth0 type veth peer name veth1
ip addr add 10.11.0.1/24 dev veth0 && ip addr add 10.11.0.2/24 dev veth1
ip link set veth0 up && ip link set veth1 up
ip neigh replace 10.11.0.2 lladdr "$(cat /sys/class/net/veth1/address)" dev veth0 nud permanent
ADSB_XDP_TEST=10.11.0.1,10.11.0.2 ctest --test-dir build/xdp -R Xdp
FEED_TRANSPORT=xdp FEED_IFACE=10.11.0.1 FEED_SUB_IFACE=10.11.0.2 scripts/check-feed.sh build/xdp
FEED_TRANSPORT=xdp FEED_RX=xdp FEED_IFACE=10.11.0.1 FEED_SUB_IFACE=10.11.0.2 scripts/check-feed.sh build/xdp
FEED_TRANSPORT=xdp OVL_FEED_HOST=10.11.0.2 OVL_PUB_IFACE=10.11.0.1 scripts/check-overload.sh build/xdp
```

The live AF_XDP tests are skipped unless `ADSB_XDP_TEST` is set, so a plain `ctest` does not exercise them. AF_XDP UMEMs count against `RLIMIT_MEMLOCK` (8 MiB on GitHub runners).

For latency, CI puts the receiver in its own network namespace so kernel UDP also crosses the veth (job `xdp`, last step, as root):

```bash
ip link add vb0 type veth peer name vb1 && ip netns add rx && ip link set vb1 netns rx
ip addr add 10.12.0.1/24 dev vb0 && ip -n rx addr add 10.12.0.2/24 dev vb1
ip link set vb0 up && ip -n rx link set vb1 up
build/xdp/tools/adsb-tx-bench --transport kernel,xdp --rx all --interface 10.12.0.1 --dest 10.12.0.2 \
  --rx-netns rx --xdp-mode copy --seconds 2 --rounds 2
```

**CI:** job `xdp` (veth pair, file capabilities instead of sudo, the feed suite both ways, the overload suite, the bench for the log).

| Result (veth, receiver in a netns, 100k/s × 1,200 B, medians) | Source | Where measured |
|---|---|---|
| Send, copy mode: `send()` p50 kernel 750 ns, AF_XDP 750 ns; send → recv 1.0 µs both | core/README.md | Docker on the laptop, CPUs pinned. **Benchmark host** |
| Receive: kernel `recv()` 1.71 µs vs AF_XDP 1.13 µs send → recv p50 | core/README.md | Same. **Benchmark host** |
| Zero-copy | not measured | Needs a NIC with zero-copy driver support: **Benchmark host** (`bench/lab/run.sh --steps xdp`) |

---

## Overload

```bash
cd core
scripts/check-overload.sh build/release
```

25 checks. The state stage is slowed to a known cost (`--simulate-work-ns 10000`, about 90k frames/s), then `adsb-beast-serve` drives `adsb-live` at 0.5×, 2× and 10× that rate and through a 10× burst: loss only at the input boundary and counted, bounded queueing delay (`--max-queue 2048`), the load source stays on schedule, constant memory, the feed's subscriber still matches the publisher; `--overload block` is run for contrast. Transport variants: `FEED_TRANSPORT=uring|xdp` (see above). **CI:** job `overload` (and inside `uring`, `xdp`).

| Result | Source | Where measured |
|---|---|---|
| Loss 0% / 50% / 90% at 0.5× / 2× / 10×, all counted; queue wait about 30–40 ms vs about 700 ms with `--overload block`; RSS about 16–18 MB | core/README.md *Overload semantics* | Laptop |
| `adsb-live` keeps up to: ≥ 2 M frames/s without publishing; about 300k frames/s with one datagram per update (`--batch-us 0`); 1 M frames/s with batching while busy (queue wait p99 24 µs); 4.4% dropped and counted at 2 M frames/s | core/README.md | Laptop, with `adsb-beast-serve` as load source; the exact commands for these throughput points are not in the docs. **Benchmark host** (the lab `core` step runs `check-overload.sh`) |

---

## Deterministic simulation

```bash
cd core
cmake --preset release && cmake --build --preset release -j
./build/release/tools/adsb-dst --seeds 40000 --start 1                       # quick worlds (the default profile)
./build/release/tools/adsb-dst --profile deep --seeds 5000 --start 1          # larger worlds
./build/release/tools/adsb-dst --seed N --trace                               # replay one seed, every event
```

Prints totals per recovery path and a `sweep hash` that must be equal across compilers for the same seeds. A failure prints the seed, its world and the replay command. `--threads T` sets the worker count; `--time-limit S` and `--keep-going` bound long sweeps.

**CI:** job `dst` builds `adsb-dst` with GCC 14 and Clang 18 and sweeps 40,000 new seeds (`--start` = run number × 1,000,000) with both; the two `sweep hash` lines must match. Nightly (`dst-nightly.yml`, 07:17 UTC): `--profile quick --seeds 100000000 --start <run number × 1e8> --time-limit 1500 --keep-going`, then the same with `--profile deep --time-limit 900` (CI limit 50 min).

| Result | Source | Where measured |
|---|---|---|
| 200,000 quick and 5,000 deep worlds pass, about 1,500 and 70 seeds/s on 8 threads; four subscriber defects found and fixed | core/README.md *How correctness is checked* | Laptop (throughput only; correctness results do not depend on the machine) |

---

## Gateway and the browser map

### Tests

```bash
cd gateway && node --test test/*.test.mjs && cd ..
gateway/scripts/check-gateway.sh core/build/release
```

`node --test test/` (a directory) fails in Node 22: pass the files. `check-gateway.sh` (26 checks): the C++ publisher over multicast with 1% loss and a 2,048-message ring; gateways that keep up, lose bursts of 4,000 datagrams, or join late; a normal and a slow WebSocket client; a publisher restart. Every gateway book and client table must equal the publisher's book. **CI:** job `gateway` (`node --test --test-timeout 60000 test/*.test.mjs`, then `FEED_IFACE=127.0.0.1 ../gateway/scripts/check-gateway.sh build/gw` from `core/`).

### Live map

From the repository root (README.md *Quick start*):

```bash
core/build/release/tools/adsb-synth -o /tmp/demo.adsbrec --aircraft 150 --duration 900 --receiver 41.88,-87.63
node gateway/bin/adsb-gateway.mjs --feed 127.0.0.1:41900 &
core/build/release/tools/adsb-publish /tmp/demo.adsbrec --feed 127.0.0.1:41900 --bind 127.0.0.1 --speed 1 --busy-wait
```

Open http://127.0.0.1:8080/ (2D map) or http://127.0.0.1:8080/3d.html (3D globe). Counters: http://127.0.0.1:8080/gateway/stats. Map tiles come from Esri, so the browser needs internet access. Unicast loopback is used because macOS loopback multicast stalls; on Linux the default group `239.19.0.1:41900` works too.

Browser stand-in with latency (gateway/README.md): `node gateway/bin/adsb-ws-client.mjs` (`--slow-ms N` for a slow client, `--window N` for flow control).

| Result | Source | Where measured |
|---|---|---|
| Publish → gateway book p50 40 µs; publish → Node WebSocket client p50 95 µs, p99 about 2 ms (200 aircraft, 10×) | gateway/README.md *Measured* | Laptop, load 5–12, medians varied 10× between runs. **Benchmark host** |
| Slow client (3 ms per frame): > 1 s behind without flow control (max 18.6 s), p50 19–25 ms with window 8 | gateway/README.md | Laptop |
| Map: 150 aircraft, markers 0 m from the latest position, one render 8 ms; 3D globe about 4 ms per update | gateway/README.md | Laptop, built-in browser pane. Render-to-pixels time is not measured |

---

## Before/after benchmark against the Fall 2025 system

Runs the Fall 2025 pipeline as delivered (from the git tag `fall2025-final`, extracted by `bench/legacy/extract-fall2025.sh` into `bench/legacy/fall2025/`), then the new pipeline, on the same synthetic traffic through readsb v3.16.21, in Docker (bench/legacy/README.md):

```bash
bench/legacy/run.sh                                   # 50, 200, 1000, 3000 aircraft, 120 s each, both systems
SCENARIOS="50 200" DURATION=60 bench/legacy/run.sh    # quick run
SCENARIOS="50 200 1000 3000 6000" bench/legacy/run.sh # adds the 6,000-aircraft run
```

| | |
|---|---|
| Runtime | About 25 minutes for the default sweep (SETUP.md says about 20) |
| Disk | About 2 GB free; image `legacy-bench:latest` |
| Output | `bench/results/legacy-baseline/report.md`, `environment.json`, `n<N>/{summary.txt,metrics.json,new_pipeline.txt}` (overwrites the committed results; set `RESULTS_DIR` to write elsewhere) |
| Options | `KEEP_RAW=1` keeps raw logs in `bench/legacy/out/`; `FALL2025_REF=<ref>` measures another ref (only on purpose) |

Check the report's host-stall warning first: if the core replay's release lag p99 is above 1 ms, the tails of both systems are unreliable.

| Result (medians, 50–3,000 aircraft) | Source | Where measured |
|---|---|---|
| Frame → map in the browser (est.): Fall 2025 1.10–1.59 s, new 69–88 ms | bench/results/legacy-baseline/README.md | Laptop, Docker. **Benchmark host** |
| Frame → WebSocket at the browser 4.8–30 ms (mostly readsb's output buffering) | same | Laptop |
| Positions that reach the map 17% → 76–78%; map error 461–540 m → 66–69 m | same | Laptop (machine-independent in structure) |
| Tails: new pipeline p99 1.3–3.1 s at 1,000/3,000 aircraft, host stalls detected | bench/results/legacy-baseline/report.md | Laptop, **unreliable**. The sweep also predates the overload work (it ran `adsb-live` with one datagram per update). **Benchmark host** |

### Decoder vs readsb, position by position

```bash
bench/legacy/compare-positions.sh 1000 60 7           # AIRCRAFT SECONDS SEED (default 300 90 5)
FIX=1 bench/legacy/compare-positions.sh 1000 60 7     # new decoder with single-bit repair
```

About a minute plus the duration, in the bench image. Output: `bench/legacy/out/positions-n<N>/summary.txt` (`positions-n<N>-fix/` with `FIX=1`). Expected at 1,000 aircraft, 60 s, seed 7: new decoder 84,495 positions vs readsb `MSG,3` + `MSG,2` 84,797 (0.36% fewer), 0 unexplained differences; with `FIX=1`, 0.9994 of readsb's positions.

The committed `report.md` predates the change that counts readsb's surface positions (`MSG,2`) in the decoder comparison; a rerun updates it.

---

## Live ingest against readsb

```bash
bench/live/run.sh
```

Builds the bench image (readsb + toolchain; `BUILD_TIMEOUT`, default 900 s, where `timeout` exists), then runs `bench/live/smoke.sh` inside it: 60 s of 300 synthetic aircraft through readsb's Beast output into `adsb-live` (one connection, no parse errors, nearly every frame recorded, no phantom tracks, nearly every aircraft tracked, replay of the recording matches), then readsb is killed and restarted and `adsb-live` must reconnect. Output: `bench/live/out/summary.txt` and `bench/live/out/build.log`. Needs Docker and the same disk as the before/after benchmark. **CI:** job `live-ingest` (CI limit 25 min).

With a real receiver (SETUP.md §5):

```bash
core/build/release/tools/adsb-live --host PI_ADDRESS --port 30005 --record /tmp/live.adsbrec --receiver 40.0392,-88.2781
```

---

## Synthetic IQ, demodulator, IQ live, A/B arbitration, carrier frequency

CI job `iq` builds pinned readsb and the IQ tools, then runs four scripts. To do the same:

```bash
git clone --depth 1 --branch v3.16.21 https://github.com/wiedehopf/readsb.git /tmp/readsb
make -C /tmp/readsb -j"$(nproc)" RTLSDR=no         # Linux; needs libncurses-dev zlib1g-dev libzstd-dev
cd core
scripts/check-iq.sh build/release /tmp/readsb/readsb       # generator, scored with readsb
scripts/check-demod.sh build/release /tmp/readsb/readsb    # our demodulator (readsb optional)
scripts/check-iq-live.sh build/release                     # adsb-live --iq
scripts/check-carrier.sh build/release                     # transponder frequency method
```

`check-iq.sh` needs readsb. `check-demod.sh` without readsb runs only the absolute checks. A 2.4 MS/s `cu8` recording is 4.8 MB per second (a 30 s scene is about 144 MB).

### Synthetic IQ generator

```bash
./build/release/tools/adsb-iqsynth -o /tmp/scene --aircraft 150 --duration 30          # /tmp/scene.sigmf-data + .sigmf-meta
./build/release/tools/adsb-iqsynth -o /tmp/scene12 --rate 12e6 --format cf32 --ppm 1.5 --reflector 40.06,-88.25,250,6
python3 scripts/iq-score.py /tmp/scene.sigmf-meta decoded.beast
```

| Result (150 aircraft, 10 s, 2.4 MS/s, readsb v3.16.21) | Source | Where measured |
|---|---|---|
| Clean messages at 15–30 dB SNR: 98.8% decoded, timestamps within 0.66 samples (0.28 µs); 7–11 false decodes per 10 s, never a DF17 | core/README.md *Synthetic IQ* | Machine-independent (output is byte-identical on every platform) |
| 40 ppm, DC offset, IQ imbalance ≥ 97%; strong multipath 48%; ground traffic < 5 km 3/488 at high gain, 483/488 at low gain | same | Same |

Never transmit these files over the air.

### Demodulator

```bash
./build/release/tools/adsb-demod /tmp/scene.sigmf-meta --beast /tmp/scene.beast
python3 scripts/iq-score.py /tmp/scene.sigmf-meta /tmp/scene.beast
```

`adsb-demod` prints its throughput (ns per sample, × real time) on stderr.

| Result | Source | Where measured |
|---|---|---|
| Messages vs readsb: busy 6,332 / 6,257; weak signals 1,136 / 1,029 (+10%); impairments 2,462 / 2,391; multipath 1,364 / 1,134; starts within 0.14 samples (readsb 0.54) | core/README.md *Demodulator* | Machine-independent (readsb varies ~0.5% run to run) |
| Speed, one thread, 2.4 MS/s, 40 s busy: 6.07 ns/sample (69× real time); readsb 5.35 ns on the same laptop | same | Laptop (A/B alternated, medians). **Benchmark host**; the plan's target is ≤ 10 cycles/sample on AVX2. Not in `bench/lab/run.sh` yet |

### IQ into the live pipeline

```bash
./build/release/tools/adsb-live --iq /tmp/scene.sigmf-meta --record /tmp/iq.adsbrec        # paced like a receiver (--iq-speed 1)
./build/release/tools/adsb-iqsynth --data - --no-meta | ./build/release/tools/adsb-live --iq - --iq-format cu8 --iq-speed 0
```

| Result (100 aircraft, `check-iq-live.sh`) | Source | Where measured |
|---|---|---|
| Unpaced: exactly the frames `adsb-demod` finds (3,545), from a file and from stdin | core/README.md *IQ into the live pipeline* | Machine-independent |
| Last sample → track state, 1.7 / 6.8 / 27.3 ms blocks: p50 1.2 / 5.2 / 20 ms, p99 2.1 / 9.2 / 37 ms | same | Laptop. **Benchmark host**, then a real RTL-SDR |
| 100× real time, 4 buffers: 77% of blocks dropped and counted | same | Laptop (depends on demodulator speed) |

### A/B arbitration

```bash
cd core
scripts/check-ab.sh build/release
./build/release/tools/adsb-synth -o /tmp/ab.adsbrec --aircraft 300 --duration 60 --lines 2 --outage 1:20:40
./build/release/tools/adsb-replay /tmp/ab.adsbrec --arbitrate
```

`check-ab.sh`: 22 checks (replay against ground truth, outages, two Beast servers into `adsb-live` with replay of the run, two IQ receivers against an independent count). **CI:** job `ab`. Cost per frame: `bench_arbiter.cpp` in the `bench` preset (`adsb_bench --benchmark_filter=Arbiter`).

| Result | Source | Where measured |
|---|---|---|
| Two IQ receivers (NF 20 dB): 744 copies matched exactly, union 1,138 frames, 16% more than the better line (979); line B's +20 ppm recovered as +20.000 ppm | core/README.md *A/B line arbitration* | Machine-independent |
| About 25 ns per frame | same | Laptop. **Benchmark host** |

### Transponder carrier frequency (study method)

```bash
cd core
scripts/check-carrier.sh build/release
```

11 checks on generator scenarios with known offsets, drifts, Doppler and receiver error. **CI:** step *Carrier frequency* in job `iq`.

| Result (60 aircraft, 60 s, 2.4 MS/s, receiver +12 ppm known) | Source | Where measured |
|---|---|---|
| Per message: error p50 23 Hz, p95 117 Hz; Doppler error p50 0.4 Hz; per aircraft: offset error p50 6 Hz, drift error p50 0.08 Hz/s | core/README.md *Transponder frequency study* | Synthetic, machine-independent |
| The study itself (offsets and drifts by aircraft type) | TODO | **Real recordings** (below) |

On a real recording (core/README.md; keep the gain below clipping). 10 minutes at 2.4 MS/s is about 2.9 GB:

```bash
rtl_sdr -f 1090000000 -s 2400000 -g 30 -n 1440000000 capture.cu8
core/build/release/tools/adsb-carrier capture.cu8 --format cu8 --rate 2.4e6 --receiver LAT,LON,ALT_M --out messages.csv
python3 core/scripts/carrier-study.py messages.csv --rx-drift 10 --types ads/aircraft-types-by-hex.json --aircraft-out aircraft.csv
```

---

## Microbenchmarks

```bash
cd core
cmake --preset bench && cmake --build --preset bench -j
./build/bench/bench/adsb_bench
taskset -c 2,3 ./build/bench/bench/adsb_bench          # Linux, pinned
```

Covers the SPSC ring, histogram, CRC/decoder/CPR, track store and engine, shm ring, demodulator stages and magnitudes, and the arbiter (`core/bench/`). **CI:** job `bench-build` runs `adsb_bench --benchmark_min_time=0.01s` only to check the benchmarks run.

| Result | Source | Where measured |
|---|---|---|
| CRC-24 syndrome 8 ns; decode one DF17 22 ns; CPR global 19–30 ns; track lookup at 4,000 aircraft 2.4 ns (flat) vs 8.1 ns (`unordered_map`); full engine per frame, 300 aircraft, 118 ns vs 124 ns naive | core/README.md *Indicative numbers* | Laptop. **Benchmark host** (lab `core` step writes `microbench.json`) |

---

## Benchmark-host suite

`bench/lab/run.sh` runs the benchmarks above on a Linux host in one command (bench/lab/README.md). Get the code there with `git clone` / `git pull` only.

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config liburing-dev python3 ethtool iputils-ping
bench/lab/run.sh --dry-run --steps all --tx-port PORT_A --rx-port PORT_B     # shows what would run; changes nothing
bench/lab/run.sh --cpus 2,3                                                  # host build test core transports
bench/lab/run.sh --cpus 2,3 --steps xdp --tx-port PORT_A --rx-port PORT_B     # two unused NIC ports cabled together; sudo
bench/lab/run.sh --steps legacy                                              # before/after benchmark (Docker, ~25 min)
```

| Step | Runs |
|---|---|
| `host` | `check-host.sh --json`, CPU, kernel, NICs |
| `build` | `release` (with tests), `bench`, `naive-baseline`, all with `-DADSB_WERROR=ON` |
| `test` | `ctest --preset release`, `check-determinism.sh`, `gen.py --check` |
| `core` | `adsb_bench` (JSON), `adsb-synth` 3,000 aircraft × 120 s, replay at max speed and `--pipeline --speed 20 --busy-wait` for the optimized and naive builds, `check-overload.sh`, `adsb-shm-bench` at 10k / 100k / 1M msg/s |
| `transports` | `adsb-tx-bench --transport all` (kernel, io_uring with and without SQPOLL) |
| `xdp` | AF_XDP send and receive, copy then zero-copy, then 10× the rate in the best mode |
| `legacy` | `bench/legacy/run.sh` |

Options: `--quick` (every step short, to check the suite works), `--tune 2-5` (applies `core/scripts/tune-host.sh` for the run and reverts it; ask first on a shared machine), `--out DIR`, `--rate`, `--size`, `--rounds`. Results: `bench/lab/runs/<UTC time>/` (`SUMMARY.md`, one log per step, `host.json`, `microbench.json`, `legacy/`) and a `.tar.gz` of it to copy back. Check `release lag` in `core.log` before trusting any tail: a p99 far above a few µs means the host stalled the threads.

Host tuning by hand: `core/scripts/tune-host.sh --dry-run --cpus 2-5` (`--apply` needs root, `--revert` undoes it, `--status` shows the state).

**Not in the suite yet** (run by hand on the host, with the commands above): demodulator speed (`adsb-demod` on a generator file), IQ-live latency (`check-iq-live.sh`), feed latency over kernel UDP (`adsb-feed-cli`) and shared memory end to end (`adsb-shm-cli`), gateway latency (`check-gateway.sh`, `adsb-ws-client.mjs`), the arbiter's cost.

---

## What needs hardware

Consistent with [TEAM.md](TEAM.md) and README_UPDATES.md §22.6.

| Hardware | Experiments waiting on it |
|---|---|
| **Linux benchmark host** (≥ 8 cores, root, isolated cores; ideally Intel RDT) | Every latency number marked **Benchmark host** above; tail analysis (explain every p99.9 outlier class); the PGO and ablation table; a 10,000-aircraft load; the noisy-neighbor isolation matrix (CAT/MBA, needs RDT) |
| **NIC with AF_XDP zero-copy** (two ports cabled together) | AF_XDP zero-copy numbers (`bench/lab/run.sh --steps xdp`); the plan's ≥ 2× lower p99 TX target |
| Hardware-timestamping NIC and a second host | Hardware timestamps and PTP (on hold; the professor said software/TSC timestamps are enough) |
| **RTL-SDR** at ≥ 2.4 MS/s (the Fall 2025 Pi setup), ideally with a GPS-disciplined reference | Real IQ recordings; demodulator sensitivity ≥ 90% of readsb's on the same recording; USB transfer-size sweep; a live `librtlsdr` source; the transponder frequency study; decoder ≥ 99.9% agreement with readsb on real traffic (or OpenSky data) |
| A second RTL-SDR | Real two-receiver A/B numbers |
| Camera and a site (CMI approach path) | Minimal camera fusion (plan §13) |
| Public Nasdaq ITCH 5.0 files (no hardware) | Workstream J |
