# Benchmark host suite

`bench/lab/run.sh` runs every benchmark of the project on a Linux benchmark host in one command and puts all results into one directory, plus a `.tar.gz` of it to copy back. Laptop numbers in the docs are medians from a loaded machine. Their tails are unreliable (see `core/README.md`). This suite produces the numbers we report.

| Step | What it measures | Needs |
|---|---|---|
| `host` | CPU, kernel, NICs (driver, queues, timestamping), `check-host.sh --json` | nothing |
| `build` | release (with tests), bench (native + LTO), naive-baseline builds | compiler, CMake |
| `test` | unit tests, determinism check, `gen.py --check` (the numbers are only worth something if these pass) | build |
| `core` | microbenchmarks (JSON), replay throughput and pipeline latency (optimized vs naive build), overload suite, shared-memory ring at 10 k / 100 k / 1 M msg/s | build |
| `transports` | feed UDP transports over loopback: kernel `sendto()`, io_uring with and without SQPOLL | build (io_uring needs `liburing-dev`) |
| `xdp` | AF_XDP send and receive over a real NIC, copy and zero-copy mode, against kernel UDP in the same rounds | two unused NIC ports cabled to each other; sudo |
| `legacy` | Fall 2025 system vs new pipeline, frame → map in the browser (`bench/legacy/run.sh`, about 25 min) | Docker, about 2 GB of disk |
| `carrier` | transponder frequency study (plan §22.5): records 1090 MHz with an RTL-SDR, then every message's carrier frequency (`adsb-carrier`) and each aircraft's offset, drift and Allan deviation (`carrier-study.py`) | an RTL-SDR (`rtl-sdr` package) and `--receiver`; about 4.8 MB/s of disk while it runs |

Default steps: `host build test core transports`, plus `xdp` when ports are given. `legacy` and `carrier` run only when asked for. A failing step does not stop the others. If the build fails, the steps that need it are skipped.

## Running it

Get the code onto the host with git only:

```bash
git clone https://github.com/ryan-air/IE421-G19-ADSB.git && cd IE421-G19-ADSB
```

Later runs only need `git pull`.

Packages (Ubuntu/Debian):

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config liburing-dev python3 ethtool iputils-ping
```

Docker is needed only for the `legacy` step, and `rtl-sdr` (`sudo apt-get install -y rtl-sdr`) only for the `carrier` step.

First see what it would do. This changes nothing:

```bash
bench/lab/run.sh --dry-run --steps all --tx-port PORT_A --rx-port PORT_B
```

Then run it. `--cpus` pins the two measuring threads; pick two cores that nothing else uses, not core 0:

```bash
bench/lab/run.sh --cpus 2,3
```

```bash
bench/lab/run.sh --cpus 2,3 --steps xdp --tx-port PORT_A --rx-port PORT_B
```

```bash
bench/lab/run.sh --steps legacy
```

`--quick` makes every step short. Use it to check that the whole suite works before a long run. `--tune 2-5` applies `core/scripts/tune-host.sh` (CPU governor, IRQs off those cores, and so on) for the run and reverts it at the end. Ask before doing that on a shared machine.

The results go to `bench/lab/runs/<UTC time>/` (ignored by git):

- `SUMMARY.md`: each step's result, plus the key lines;
- one log per step;
- `host.json`, `microbench.json`;
- `legacy/` for the legacy step.

Copy the `.tar.gz` back for analysis, for example from your own machine:

```bash
scp LAB_HOST:IE421-G19-ADSB/bench/lab/runs/RUN.tar.gz .
```

## The carrier step

```bash
bench/lab/run.sh --steps carrier --receiver LAT,LON,ALT_M
```

- **What it does:** records 10 minutes (`--sdr-seconds`) at 2.4 MS/s, then runs `adsb-carrier` and `carrier-study.py --rx-drift 10 --types ads/aircraft-types-by-hex.json`. The receiver's ppm is estimated from the aircraft, and its drift is removed.
- **`--receiver`:** the antenna's latitude, longitude and height above sea level in metres. Every message's Doppler is computed from it.
- **Gain (`--sdr-gain`, default 30 dB):** the log reports the share of frames with clipped samples. Above 5% it warns: lower the gain, since clipping bends the phase of strong aircraft. Close to the antenna, too much gain matters more than too little.
- **An existing recording:** `--iq FILE` (SigMF, or raw cu8 at 2.4 MS/s) analyses it instead of recording.
- **What comes back:** `carrier/messages.csv` (one row per message), `aircraft.csv`, `study.txt` and `study.json`. The raw IQ (about 2.9 GB for 10 minutes) is deleted after the analysis unless `--keep-iq`, and is never put in the `.tar.gz`.
- **Lost samples:** if `rtl_sdr` loses samples (USB), the log says so. Message times and Doppler around the gaps are then off.

For a longer study, run it at different times of day. The drift over a flight and differences between days come from comparing runs (Allan deviation in `aircraft.csv`).

## The xdp step

The `xdp` step needs two ports, ideally on a NIC whose driver supports AF_XDP zero-copy (Intel `ice`, `i40e`, `ixgbe`, `igc`; Mellanox `mlx5`). The two ports are cabled to each other, port to port, with nothing else on them. To find candidates, list the ports and their drivers:

```bash
ip -br link
```

```bash
ethtool -i PORT
```

A candidate port shows no address in `ip -br addr`.

For the run only, the script does the following, and puts everything back on exit, including after Ctrl-C:

1. It gives the ports the test addresses 10.77.0.1 and 10.77.0.2.
2. It moves the receiving port into a network namespace. Then kernel UDP crosses the cable instead of short-cutting through the loopback device, so the kernel and AF_XDP paths are compared over the same wire.
3. It sets both ports to one queue and MTU 1500 if they are larger.

It refuses any port that is not a physical NIC port or that is in use: an IP address, a route, a bond or bridge, or the address you are logged in through. It also refuses if 10.77.0.0/24 is already used on the machine.

The step runs copy mode first, then zero-copy. If the driver cannot do zero-copy, the log says so and the copy-mode numbers stand. Then it runs at ten times the rate in the best mode that worked. The report line under each result says which mode the kernel granted at each end and whether the XDP program attached in driver or generic mode.

## Interpreting

- Compare transports only within one run. The suite alternates them round by round, so a change in machine load affects all of them alike.
- `core.log` prints `release lag` for every paced replay. A p99 far above a few µs means the host stalled the threads, and that run's tails should not be trusted. Pin cores (`--cpus`), tune (`--tune`), and rerun.
- `send->recv` uses one clock on both ends (same host), so it needs no clock synchronization.
