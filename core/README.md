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
| Mode S decoder | [`adsb/modes/decoder.hpp`](include/adsb/modes/decoder.hpp) | Frame → fixed-size `DecodedMsg`: DF0/4/5/11/16/17/18/20/21/24, ADS-B TC 0–22, 28, 29 (target state) and 31 (operational status), Comm-B registers 1,0 1,7 2,0 3,0 4,0 5,0 6,0 (inferred), optional single-bit repair |
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
| Feed messages | [`adsb/protocol/feed_messages.hpp`](include/adsb/protocol/feed_messages.hpp) | **Generated** from [`protocol/schema/feed_v1.json`](../protocol/schema/feed_v1.json); see [`protocol/feed_spec.md`](../protocol/feed_spec.md) |
| MoldUDP64 framing | [`adsb/protocol/moldudp64.hpp`](include/adsb/protocol/moldudp64.hpp) | Packet builder, bounds-checked parser, subscriber `SequenceTracker` (gap / overlap / duplicate / heartbeat / new session / malformed: seq 0 or past 2^64 − 1) |
| A/B arbiter | [`adsb/ingest/arbiter.hpp`](include/adsb/ingest/arbiter.hpp) | First copy of each frame forwarded, later copies from other lines dropped and counted, per-line contribution, win margin, relative radio clock (see *A/B line arbitration*) |
| Replay | [`adsb/replay/replay.hpp`](include/adsb/replay/replay.hpp) | Inline and two-thread pipeline replay at max or paced speed, per-stage latency histograms, SHA-256 of the output stream |
| Naive baseline | [`adsb/common/build_config.hpp`](include/adsb/common/build_config.hpp) | `-DADSB_NAIVE=ON` swaps `SpscRing` → mutex queue and the flat table → `std::unordered_map`. The pipeline code is identical. |

### Feed distribution (Workstream E, kernel UDP transport)

| Component | Header | What it is |
|---|---|---|
| Publisher | [`adsb/feed/publisher.hpp`](include/adsb/feed/publisher.hpp) | The track engine's sink. Sequences each message, packs MoldUDP64-style datagrams (immediate, or batched up to N µs), sends heartbeats when quiet, repeats end-of-session. Hands every message to the recovery thread through a lock-free SPSC log; no allocation or locks on the hot path. Includes deterministic loss injection (`LossyOut`). |
| Recovery servers | [`adsb/feed/recovery.hpp`](include/adsb/feed/recovery.hpp) | On their own thread: a ring of the last 65,536 messages for **retransmit** (unicast UDP; a header-only NAK when the range is gone) and the publisher's book for **snapshots** (TCP, non-blocking writes to up to 16 clients). The logic (`RecoveryState`) is separate from the sockets so tests drive it directly. |
| Subscriber | [`adsb/feed/subscriber.hpp`](include/adsb/feed/subscriber.hpp) | `SubscriberCore`: the recovery protocol of [`spec/FeedRecovery.tla`](../spec/FeedRecovery.tla) as code, plus sessions (publisher restarts, stale packets, late join). Buffers ahead of gaps, requests retransmits, falls back to snapshots. No sockets or clock of its own. `SubscriberClient` adds the sockets (multicast or unicast feed, request socket, blocking TCP snapshot fetch). |
| Deterministic simulation | [`adsb/feed/dst.hpp`](include/adsb/feed/dst.hpp) | The real `Publisher`, `RecoveryState` and `SubscriberCore` in one process on a simulated network and clock (§12). A discrete-event engine orders every datagram, timer, recovery-thread pass, snapshot fetch, crash and restart; one seed determines the whole run, so a failure replays exactly (`adsb-dst --seed N --trace`). Each seed draws its own world: 1–6 subscribers joining at random times with blocking (like `SubscriberClient`) or asynchronous (like the gateway) snapshot fetches; per-link loss, outage-length bursts, duplication, jitter and long delay spikes; recovery-thread stalls; ring and reply-cap sizes; failed and cut snapshots; publisher crashes and clean restarts with downtime. Checked after every step: each applied message is byte-identical to the published one, in order; the book equals the publisher's as of `expected − 1`; a subscriber is never live in an older session than one it was live in. After the faults stop: every subscriber ends the final session with the publisher's exact book (SHA-256) within 20 s simulated, or later while it keeps advancing (slow configurations, up to 200 s), failing two seconds after it stops (liveness), and no run exceeds an event budget (storms). |
| Book and snapshot stream | [`adsb/feed/book.hpp`](include/adsb/feed/book.hpp), [`adsb/feed/snapshot.hpp`](include/adsb/feed/snapshot.hpp) | The table a feed describes (ICAO → latest `TrackUpdate`) with a SHA-256 digest over its wire bytes, and the validated snapshot stream format |
| Sockets | [`adsb/feed/net.hpp`](include/adsb/feed/net.hpp) | IPv4 UDP multicast/unicast send and receive (group join, TTL 1, loopback), TCP listener |
| Shared-memory ring | [`adsb/feed/shm_ring.hpp`](include/adsb/feed/shm_ring.hpp), [`adsb/feed/shm_subscriber.hpp`](include/adsb/feed/shm_subscriber.hpp) | The same-host transport (§9.3): a single-producer, multi-consumer ring on a POSIX shared-memory object, a seqlock per 128-byte slot. The producer never waits; each consumer keeps its own cursor and detects a lap from the sequence numbers. `ShmOut` plugs into the publisher (it shares sequencing and the recovery log with UDP); `ShmSubscriber` keeps the same `FeedBook`, joins from the ring or a TCP snapshot, resyncs after a lap, and follows a restarted publisher to its new ring. A snapshot is used only if it belongs to the ring's session, the ring behind the name is still the one being read, and the ring still holds the next message; a subscriber lapped after the end with no snapshot available reports that it gave up instead of waiting forever. |
| io_uring transport | [`adsb/feed/uring_sender.hpp`](include/adsb/feed/uring_sender.hpp), [`adsb/feed/tx.hpp`](include/adsb/feed/tx.hpp) | The publisher's UDP datagrams through io_uring (§9.3), chosen with `--transport kernel\|uring\|uring-nosqpoll` on `adsb-publish` and `adsb-live`. `send()` copies into a fixed buffer pool and queues an `IORING_OP_SEND` on the registered, connected socket; with SQPOLL a kernel thread picks it up, so the hot thread makes no syscall per datagram. Every send is `MSG_DONTWAIT`: when the socket buffer is full, io_uring would otherwise park the request and let later datagrams overtake it (a unit test saw thousands of reorders that way, even on an `O_NONBLOCK` socket); now it fails with `EAGAIN`, counted, like a non-blocking `sendto()`, and retransmit recovers it. Linux with liburing (`ADSB_IO_URING=AUTO\|ON\|OFF`); elsewhere a stub. |
| AF_XDP transport | [`adsb/feed/xdp_sender.hpp`](include/adsb/feed/xdp_sender.hpp), [`adsb/feed/tx.hpp`](include/adsb/feed/tx.hpp) | The publisher's datagrams as whole Ethernet + IPv4 + UDP frames on an XSK TX ring (§9.3), `--transport xdp` on `adsb-publish` and `adsb-live` (`--xdp-mode auto\|copy\|zerocopy`, `--xdp-queue`, `--xdp-frames`, `--xdp-dst-mac`). `--interface ADDR` names the sending device and is the source IP; the destination MAC is the group's multicast MAC, `--xdp-dst-mac`, or the ARP table. Headers come from a prebuilt template with the IPv4 and UDP checksums computed per frame (checked against RFC 1071 and a known header, and by the receiving kernel in every live test). The TX ring is FIFO and an unsent descriptor stays queued for the next kick, so datagrams never overtake each other. Frames leave through the device only: no copy to subscribers on the same host (they use the shm ring). Raw `<linux/if_xdp.h>` syscalls, no libbpf/libxdp (TX needs no XDP program); `ADSB_AF_XDP=AUTO\|ON\|OFF`, Linux only. Needs `CAP_NET_RAW`. The subscriber side is the next row. |
| AF_XDP reception | [`adsb/feed/xdp_receiver.hpp`](include/adsb/feed/xdp_receiver.hpp) | The subscriber side (§9.3): an XDP program on the receiving device redirects the feed's UDP datagrams into an XSK, so they skip the kernel's IP/UDP stack. The program is about 25 eBPF instructions built in C++ (`xdp_feed_program`) and loaded with the raw `bpf()` syscall: no clang BPF target, no libbpf. It redirects Ethernet + IPv4 (no options, not a fragment) + UDP to the feed's address and port to `XSKMAP[rx queue]`, and passes everything else to the kernel, including feed frames on a queue with no socket (`bpf_redirect_map`'s fallback). Attached through a BPF link (kernel 5.9+), so it detaches when the subscriber exits or crashes; native (driver) mode if the driver supports XDP, else generic. `XdpReceiver` owns the UMEM (`--xdp-frames`, all on the fill ring), checks each frame (`udp_payload`: IPv4 header checksum, lengths, Ethernet padding; the UDP checksum is not verified, since same-host senders may leave a partial one) and returns it to the fill ring at once. `open()` refuses a device whose MTU does not fit a UMEM frame after the kernel's headroom (a redirected frame that does not fit is dropped, not passed on). `SubscriberClient` with `rx = kXdp` (`adsb-feed-cli --rx xdp --interface ADDR`, `--xdp-queue/-mode/-attach/-frames`) polls the XSK next to its kernel socket, which keeps the group membership and receives anything the program passes on; the report's `rx` line splits datagrams by path. One XSK per device queue and one XDP program per device: one AF_XDP feed handler per NIC (local consumers would read the shm ring). Needs `CAP_NET_ADMIN`, `CAP_BPF` and `CAP_NET_RAW`. |

**io_uring transport, measured** (`adsb-tx-bench`, 1,200-byte datagrams at 100 k/s on loopback, Linux 6.10 in Docker on the M2 laptop, load average 10–13: medians are stable across runs, tails are not and are not reported):

| Transport | `send()` call, p50 | send → recv, p50 | Syscalls per datagram | Cores busy (sender + receiver + SQ thread) |
|---|---|---|---|---|
| kernel `sendto()` | 1,459 ns | 1.5 µs | 1 | 2.0 |
| io_uring, SQPOLL | **208 ns** | 1.7 µs | 0 (3–5 wakeups per run) | 3.0 |
| io_uring, no SQPOLL | 1,625 ns | 1.6 µs | 1 (`io_uring_enter`) | 2.1 |

SQPOLL takes the send off the hot thread (7× cheaper there) at the price of a polling core; delivery itself is not faster on loopback (one more hop, through the kernel thread). Without SQPOLL io_uring is slightly slower than `sendto()`. Integration: `check-feed.sh` (all 15 checks) passes over both io_uring modes, and `check-overload.sh` (25) with `adsb-live --transport uring`, with 0 failed and 0 reordered sends. TX-to-wire latency needs the benchmark host's NIC and hardware timestamps (§9.4).

**AF_XDP copy mode, measured** (`adsb-tx-bench --rx-netns`, same rate and size, over a veth pair whose far end is in its own network namespace so every transport crosses the same device; Linux 6.10 in Docker on the laptop, CPUs pinned, 5 alternating rounds of 3 s; medians):

| Transport | `send()` call, p50 | send → recv, p50 |
|---|---|---|
| kernel `sendto()` | 750 ns | 1.0 µs |
| AF_XDP, copy mode | 750 ns | 1.0 µs |
| io_uring, SQPOLL | 125 ns | 1.1 µs |
| io_uring, no SQPOLL | 1,043 ns | 1.0 µs |

In copy mode AF_XDP gains nothing over `sendto()`: each kick is still a syscall, and the kernel still allocates an skb and copies the frame; only the IP/UDP layers are skipped. (A first run gave kernel 1,209 ns / 1.5 µs against AF_XDP 791 ns / 1.1 µs, but the kernel's turn overlapped a 23 ms host stall; the repeat was identical.) The plan's AF_XDP target (≥ 2× lower p99 TX) needs zero-copy, i.e. a NIC whose driver supports it, on the benchmark host. Integration: `check-feed.sh` (15 checks + 3 AF_XDP counter checks) and `check-overload.sh` (25) pass with the publisher on AF_XDP over the veth pair, with 0 invalid descriptors and 0 failed kicks.

**AF_XDP reception, measured** (`adsb-tx-bench --rx all`, same setup as above: the receiver in its own namespace, CPUs pinned, 5 alternating rounds of 2 s at 100,000 datagrams/s of 1,200 bytes; medians):

| Send → receive path | send → recv, p50 | lost |
|---|---|---|
| kernel `sendto()` → kernel `recv()` | 1.71 µs | 0 |
| kernel `sendto()` → AF_XDP | 1.13 µs | 0 |
| AF_XDP → kernel `recv()` | 1.71 µs | 989 |
| AF_XDP → AF_XDP | 1.13 µs | 0 |

Receiving through the XSK takes about a third off the median (0.6 µs: no skb through the IP/UDP layers, no socket queue, no `recv()` syscall per datagram), even in copy mode. The program attached in driver mode on veth. Losses were only ever on the kernel socket, whose buffer a container caps at about 200 KB (a first run lost 574 and 1,677 there, against 0 on AF_XDP with its 2,048-frame ring). Tails on this laptop are host stalls and not comparable. Integration: `check-feed.sh` with `FEED_RX=xdp` passes with the subscribers receiving every feed datagram through AF_XDP (32,540 and 38,199, none through the kernel) across loss recovery and a publisher restart.

To run the AF_XDP tests (Linux; `CAP_NET_ADMIN` for the veth pair, `CAP_NET_RAW` for the socket, plus `CAP_NET_ADMIN` and `CAP_BPF` for the receiving side's XDP program; in Docker add `--cap-add NET_ADMIN --sysctl net.ipv4.conf.all.accept_local=1 --sysctl net.ipv4.conf.all.rp_filter=0`, since both ends live in one namespace):

```bash
ip link add veth0 type veth peer name veth1
ip addr add 10.11.0.1/24 dev veth0 && ip addr add 10.11.0.2/24 dev veth1
ip link set veth0 up && ip link set veth1 up
ip neigh replace 10.11.0.2 lladdr "$(cat /sys/class/net/veth1/address)" dev veth0 nud permanent
ADSB_XDP_TEST=10.11.0.1,10.11.0.2 ctest --test-dir build/release -R Xdp
FEED_TRANSPORT=xdp FEED_IFACE=10.11.0.1 FEED_SUB_IFACE=10.11.0.2 scripts/check-feed.sh build/release
FEED_TRANSPORT=xdp FEED_RX=xdp FEED_IFACE=10.11.0.1 FEED_SUB_IFACE=10.11.0.2 scripts/check-feed.sh build/release
```

**Shared-memory ring, measured** (M2 laptop, not the benchmark host; no core isolation):

| Measurement | Result |
|---|---|
| Write / read one message, one thread | 2.5–4.3 ns / 1.8–3.0 ns (`adsb_bench --benchmark_filter=Shm`) |
| Ring alone, write → read on another thread (`adsb-shm-bench`), 100 k msg/s | p50 125 ns, p99 209 ns (target p99 < 500 ns). The 125 ns median is three ticks of the 24 MHz timer. Tails (p99.9 tens of µs) are the OS descheduling threads; at 10 k msg/s p99 was 750 ns (caches go cold between messages). |
| End to end, `adsb-publish --shm` → `adsb-shm-cli` (book applied), 300 aircraft at 10× | p50 125 ns, p99 3.2–3.7 µs, p99.9 11 µs. The extra over the ring alone is the publisher's other work and the subscriber's `FeedBook` (a hash map) update. |
| Integration (`scripts/check-shm.sh`) | 13/13 checks: a subscriber that keeps up (no lap, no snapshot), one that is lapped 12 times and resyncs each time, a late joiner, and a publisher restart; every book equals the publisher's |

Final numbers need the benchmark host with pinned, isolated cores (`adsb-shm-bench --cpus W,R`).

### Overload semantics (Workstream F, README_UPDATES.md §10)

What happens when the input outruns the pipeline is defined at each boundary and tested:

| Boundary | Policy | Where |
|---|---|---|
| SDR / IQ file → demodulator (`adsb-live --iq`) | **Drop the whole block and count it** when every slot of the IQ block ring is in use (an SDR cannot be paused). The demodulator is told of the gap (`Demodulator::skip`), so frames after it keep their place on the sample clock; a frame cut by the gap is lost. Unpaced file input (`--iq-speed 0`) waits instead. | [`adsb/ingest/iq_ring.hpp`](include/adsb/ingest/iq_ring.hpp), [`iq_source.hpp`](include/adsb/ingest/iq_source.hpp); the report gives blocks and samples dropped and the ring's high-water mark |
| Live input → decode/state (`adsb-live`) | **Drop the frame and count it** when `--max-queue` frames are already waiting (default: the queue's 65,536). The ingest thread keeps reading the socket, so the upstream receiver is never pushed back. `--overload block` restores the old behavior (stop reading; the loss then happens upstream, uncounted). | `tools/adsb_live.cpp`: the one place a frame may be lost; the report gives the count and the queue's high-water mark |
| Decode/state → publish → recovery log | Lossless (internal). The publisher batches **only while busy**: a datagram is sent the moment the input queue runs dry, so light load adds no delay, and under load it fills up to 22 messages, at most `--batch-us` (100 µs) old. | `publisher.hpp`, `adsb_live.cpp` |
| Publisher → network | Never blocks: a full socket buffer is a counted send failure; subscribers recover by retransmit. | `net.hpp` (non-blocking UDP) |
| Gateway → browsers | Per-client conflation and credit flow control: a slow client gets fewer, current frames. | [`gateway/`](../gateway/README.md) |
| Conflated feed (§10.2) | A second, ordinary feed (own group, session, sequence numbers, retransmit and snapshot servers) carrying only the latest state per aircraft per tick (default 10 Hz). After every tick its book equals the main feed's, by state (`FeedBook::state_digest()`). Each tick is packed into full datagrams. | [`adsb/feed/conflate.hpp`](include/adsb/feed/conflate.hpp) (the `Conflator`: one hash lookup and a 72-byte copy per update, no allocation; aircraft beyond its capacity pass through unconflated, and after any such overflow every delete is sent, since "never sent" can no longer be known), [`conflated_feed.hpp`](include/adsb/feed/conflated_feed.hpp); `--conflate GROUP:PORT` on `adsb-live` and `adsb-publish` |

Measured on the M2 laptop with `adsb-beast-serve` (below) as the load source, 3,000-aircraft traffic:

| `adsb-live` configuration | Keeps up to | Above that |
|---|---|---|
| Decode + track state, no publishing | ≥ 2 M frames/s (the load source's limit); core alone replays at 1.9–2.0 M frames/s | – |
| Publishing, one datagram per update (`--batch-us 0`, the old default) | about 300 k frames/s | old policy: queue full (~210 ms waits), load source blocked for seconds |
| Publishing, batching while busy (new default) | 1 M frames/s, queue wait p99 24 µs, max 0.4 ms, no drops | 2 M frames/s: 4.4% dropped and counted, load source on schedule |

`scripts/check-overload.sh` slows the state stage to a known cost (`--simulate-work-ns 10000`, about 90 k frames/s) and checks 0.5×, 2× and 10× saturation and a 10× burst. Loss is 0%, 50% and 90%, all at the input boundary and counted. Queue wait stays under about 30–40 ms (`--max-queue 2048`), against about 700 ms with `--overload block`. The load source stays on schedule, memory stays at about 16–18 MB at every load, dropping stops when a burst ends, and the feed's subscriber still reconstructs the publisher's book byte for byte. The old blocking policy is run for contrast: no counted loss, but the load source falls more than a second behind.

### Synthetic IQ (Workstream A, README_UPDATES.md §22.4)

[`adsb/sim/iq.hpp`](include/adsb/sim/iq.hpp) turns the synthetic traffic into the complex baseband samples a 1090 MHz receiver would record, with the truth for every message. It is the test input for our demodulator (next section), and a scenario generator for testing other SDR receivers, as the professor asked (§22.1).

| Effect | Model |
|---|---|
| Waveform | Mode S PPM: preamble pulses at 0, 1, 3.5, 4.5 µs, then 56 or 112 bits of 1 µs; trapezoidal pulses (rise 0.08 µs, fall 0.12 µs by default); carrier phase continuous across the message |
| Transmitter | Per aircraft: power 125–500 W, carrier offset from 1090 MHz (Gaussian, default σ 100 kHz, clamped to the ±1 MHz tolerance), linear drift; per message: jitter. All of it is in the truth |
| Geometry | WGS-84 positions; free-space path loss; propagation delay; vertical dipole receive antenna (2.15 dBi at the horizon, a null overhead) or isotropic |
| Doppler | From the range rate along the line of sight, f_rx = f_tx (1 − ṙ/c): about ±0.9 kHz at 250 m/s |
| Atmosphere | Effective-Earth radio horizon (k = 4/3; larger k models ducting), then a diffraction loss per km beyond it (default 1 dB/km), so aircraft fade out below the horizon. Gaseous absorption and rain are under 0.01 dB/km at L band and are left out |
| Multipath | Two-ray ground reflection over the same effective Earth as the horizon: the specular point (found by bisection), the path difference, and the divergence factor of a convex surface; \|Γ\| with phase π. Up to six fixed specular reflectors (buildings, hills) give delayed echoes, each with the Doppler of its own path and the diffraction loss of each leg beyond its horizon. A delayed copy's carrier phase comes from the LO frequency (x(t − τ)·e^(−j2π·f_LO·τ)) |
| Collisions | Overlapping replies add; each annotation says whether it collided and how strong the strongest interferer was |
| Receiver | One crystal for LO and sample clock (`--ppm`): it shifts every frequency and stretches sample time; thermal noise from the noise figure over the sample bandwidth; DC offset; IQ gain and phase imbalance; ADC clipping and quantization (`cu8` for RTL-SDR and readsb, `cs8`, `cs16`; `cf32` unquantized). `--full-scale` sets the gain: high gain (default −70 dBm) clips nearby aircraft, low gain loses distant ones in the 8-bit quantization, as with a real dongle |

Each output sample averages 8 points over its sample period (the integrate-and-dump of a decimating receiver), so arrival times and pulse edges fall anywhere between samples. Any rate works (2.0, 2.4 and 12 MS/s are tested). Output is a [SigMF](https://sigmf.org) recording: `NAME.sigmf-data` and `NAME.sigmf-meta`. The metadata has the configuration, every transmitter (power, offset, drift: the truth for the frequency-stability study, §22.5), and one annotation per message: sample position, hex, SNR, received power, range, elevation, range rate, transmitter offset, Doppler, the frequency as it appears in the samples, collisions, and the aircraft's position.

The output is the same bytes on every platform and for any thread count: the signal path uses [`portable_math.hpp`](include/adsb/common/portable_math.hpp) (sin, cos, log, exp and atan from + − × ÷ only, since libm results differ between platforms in the last bit), and noise is seeded per 65,536-sample chunk. `test_iq.cpp` pins a hash of a whole scenario; it is equal on macOS Clang, Linux GCC 14 and Clang 18.

**Checked against readsb** ([`scripts/check-iq.sh`](scripts/check-iq.sh), CI job `iq`). Pinned readsb v3.16.21 decodes the files, and its Beast timestamps (a 12 MHz count of samples) are matched to the truth by hex and time ([`scripts/iq-score.py`](scripts/iq-score.py)):

| Scenario (150 aircraft, 10 s, 2.4 MS/s) | Result |
|---|---|
| Clean messages at 15–30 dB SNR, `cu8` and `cs16` | 98.8% decoded; every timestamp within 0.66 samples (0.28 µs) of the truth |
| False decodes | 7–11 in 10 s, never a DF17: readsb's own, almost all DF11 replies with bit errors in the parity field, which readsb reads as an interrogator code |
| 40 ppm clock error, DC offset, 5% / 3° IQ imbalance | still ≥ 97% |
| Strong multipath (\|Γ\| 0.7, a reflector 3 dB down) | 48% decoded; 95% of readsb's extra decodes are the echoes themselves, microseconds late |
| Ground traffic within 5 km | 3/488 decoded at high gain (clipped), 483/488 at low gain |

Below 15 dB, readsb's rate falls off (about 20–50% at 10–15 dB): SNR here is peak power over the noise in the whole 2.4 MHz band. It does not include the small loss of the sample averaging at large offsets (−2.7 dB at 1 MHz offset at 2.4 MS/s; negligible at the default 100 kHz spread). Our own demodulator is scored the same way (next section).

Inputs are validated (`validate_iq_config`): every value must be finite and physical, since out-of-range ones (a full scale of −5000 dBm, say) would give infinities. A recording lasts the traffic plus `--tail`, extended so that every message, with its propagation delay and echoes, ends inside it. readsb's `--raw` output has no timestamps, and DF11 replies repeat the same hex, so a hex-only match charges a missed early copy to the last one; the scorer therefore matches by hex and time on readsb's Beast output. At rates below about 2.4 MS/s, offsets near ±1 MHz alias (the annotation's frequency edges assume the signal is in band).

```bash
./build/release/tools/adsb-iqsynth -o /tmp/scene --aircraft 150 --duration 30          # cu8, 2.4 MS/s
./build/release/tools/adsb-iqsynth -o /tmp/scene12 --rate 12e6 --format cf32 --ppm 1.5 --reflector 40.06,-88.25,250,6
./build/release/tools/adsb-iqsynth --data - --no-meta | readsb --device-type ifile --ifile - --iformat UC8 --raw
python3 scripts/iq-score.py /tmp/scene.sigmf-meta decoded.beast    # or a --raw text dump (matched by hex only)
```

**Never transmit these files over the air.** Playing them into a receiver is only legal by cable through attenuators or in a shielded enclosure; transmitting on 1090 MHz interferes with air traffic control.

Not modelled yet: Mode A/C replies (the other half of real 1090 MHz traffic), aircraft antenna patterns and fuselage shadowing, terrain, intra-message frequency chirp, and the RTL-SDR's own filter shape. MATLAB's receiver example (§22.3) has not been run on the files (no MATLAB licence on hand).

### Tools and scripts

| Tool | Purpose |
|---|---|
| `build/<preset>/tools/adsb-synth` | Generate synthetic traffic: `.adsbrec`, plus optional raw Beast stream and ground-truth CSV; `--lines 2` for two receivers, `--outage L:FROM:TO` to unplug one |
| `build/<preset>/tools/adsb-iqsynth` | Synthetic 1090 MHz IQ recordings with per-message truth (SigMF): see *Synthetic IQ* above |
| `build/<preset>/tools/adsb-demod` | Demodulate an IQ recording (SigMF, raw file or stdin) into Mode S frames, as Beast binary or text: see *Demodulator* above |
| `build/<preset>/tools/adsb-replay` | Replay a recording through decode + track state: counters, output hash, optional pipeline latency; `--arbitrate` merges A/B lines first |
| `build/<preset>/tools/adsb-live` | Run the pipeline on a live readsb feed (ingest → track state, optional recording, optional `--publish` to the feed), with receive → state latency and the overload counters (`--overload`, `--max-queue`); or on IQ samples through our demodulator (`--iq`: see *IQ into the live pipeline*); two receivers with `--b-port` or `--iq-b` (see *A/B line arbitration*) |
| `build/<preset>/tools/adsb-shm-cli` | Subscribe on the same host through the shared-memory ring (`--name /adsb-feed`): joins, recovers from laps and restarts, reports publish → applied latency and the book digest. Publish with `--shm NAME` on `adsb-live` or `adsb-publish` |
| `build/<preset>/tools/adsb-shm-bench` | Latency of the shared-memory ring alone: a paced writer thread and a spinning reader (`--rate`, `--cpus W,R`) |
| `build/<preset>/tools/adsb-ring-stress` | Long concurrent stress run of the SPSC ring and the shm ring (`--seconds`, `--seed`): order, exactly-once delivery, torn contents, laps. Run it on ARM; see `spec/memory-model/README.md` |
| `build/<preset>/tools/adsb-tx-bench` | The feed's UDP transports on identical histograms: kernel `sendto()` vs io_uring with and without SQPOLL vs AF_XDP (`--transport all --rounds N` alternates them; `--interface`/`--dest`/`--rx-netns` to cross a real device such as a veth pair; `--rx kernel\|xdp\|all` receives with `recv()`, AF_XDP, or both, crossed with every transport); `send()` cost, send → recv latency, busy cores, loss and reorders |
| `build/<preset>/tools/adsb-dst` | Deterministic simulation sweep: `--seeds N --start S --threads T` (`--profile deep` for larger worlds, `--time-limit` for budgets); prints totals per recovery path and a sweep hash (equal across compilers); a failure prints the seed, its world and the replay command (`--seed S --trace` prints every event) |
| `build/<preset>/tools/adsb-beast-serve` | Serve a recording as a Beast TCP stream (like readsb's output) at a fixed rate, with optional bursts; reports whether the consumer pushed back. The load source for overload tests; `--line N` serves one receiver line (one server per line for A/B) |
| `build/<preset>/tools/adsb-publish` | Replay a recording through decode + track state and publish it as the feed, with the retransmit and snapshot servers; fault injection; prints the final book digest |
| `build/<preset>/tools/adsb-feed-cli` | Subscribe: verify sequencing, recover gaps, report counters and publish → applied latency, print the reconstructed book's digest; `--rx xdp` receives the feed through AF_XDP |
| `build/<preset>/tools/adsb-clock` | Answers clock queries on stdin with `mono_ns()`, so the Node gateway can measure latency against the publisher's clock ([gateway/](../gateway/README.md)) |
| [`scripts/check-feed.sh`](scripts/check-feed.sh) | Feed integration test: loss, small ring, late join, publisher restart; every subscriber's book must equal the publisher's and a plain replay's (CI) |
| [`scripts/check-shm.sh`](scripts/check-shm.sh) | Shared-memory transport test: a subscriber that keeps up, one that is lapped, a late joiner, a publisher restart; every book must equal the publisher's (CI) |
| [`scripts/check-conflate.sh`](scripts/check-conflate.sh) | Conflated feed test: subscribers that keep up, lose bursts, or join late, from `adsb-publish` and `adsb-live`; every conflated book must equal the main feed's state, and the conflated feed must send less (CI) |
| [`scripts/check-overload.sh`](scripts/check-overload.sh) | Overload test: 0.5×, 2×, 10× saturation and a burst; loss only at the input boundary and counted, bounded queueing delay, no upstream pushback, constant memory, feed still consistent (CI) |
| [`scripts/check-ab.sh`](scripts/check-ab.sh) | A/B arbitration test: replay against ground truth, outages, live from two Beast servers (decisions repeat on replay), two IQ receivers against an independent count (CI) |
| [`scripts/check-iq.sh`](scripts/check-iq.sh) | IQ generator test: determinism, formats, SigMF, and readsb decoding the files, scored against the truth (CI) |
| [`scripts/iq-score.py`](scripts/iq-score.py) | Scores any decoder's output (Beast with timestamps, or `--raw` text) against an IQ file's truth, by SNR band and collisions |
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

### Demodulator (Workstream A, README_UPDATES.md §5.2)

[`adsb/demod/demod.hpp`](include/adsb/demod/demod.hpp) turns complex baseband samples into CRC-checked Mode S frames, at any rate from 2 MS/s up, in `cu8`, `cs8`, `cs16` or `cf32`. It works on magnitudes, so carrier offsets (transmitter tolerance, Doppler, receiver ppm) do not matter. Per sample position:

1. **Gate** (branch-free over a whole block, vectorized): for each of the four preamble pulses, one of the two windows that can hold it must sum to 0.9 of what the weakest accepted pulse would put there. Noise passes this a few percent of the time.
2. **Preamble**, at 5 fractional starts per sample: the mean of the four pulses must reach twice the noise floor, and each pulse 1.2 times the mean of the quiet parts. A pulse's energy is the integral of the sampled magnitude over its interval, from precomputed weights (four multiply-adds per half-bit up to 6 MS/s).
3. **Bits**: 1 if the first half-bit holds more energy than the second. At 2.4 MS/s a sample can straddle two half-bits and then counts toward both, which costs contrast. When no phase gives a valid frame, a **sequence detector** fits the pulse level and noise floor on the preamble and finds the most likely bits with a 2-state Viterbi over a model of every sample (its expected magnitude √((A·x)² + μ²), x its overlap with the pulses). This is what makes the demodulator more sensitive than readsb (below).
4. **Check**: DF17/18 need a zero CRC syndrome, DF11 a syndrome below 80 (an interrogator code; a clean reading at another phase is preferred); the Address/Parity formats (DF0/4/5/16/20/21, parity = address) need an address that a verified DF11/17/18 frame showed in the last 60 s. `--fix` corrects one wrong bit in DF11/17/18 (never in the format bits); off by default, like readsb with `--no-fix`. The sequence detector's frames must pass with a clean CRC or a known address.
5. **Timing**: a matched filter (the expected magnitude of every sample for a trial start, correlated and normalized, so the peak does not depend on the sampling phase), climbed in 0.04 µs steps and refined with a parabola; it also gives the pulse amplitude. Scanning resumes 16 µs before the frame's end, since the next reply may start under its tail (busy channels).

The **noise floor** comes from 16-sample window means: per 1,024-sample block, the mean of the lowest quarter of its 64 window means (robust to quantization when the noise is about one ADC step, and to replies filling up to about three quarters of the time), and a position uses the smallest estimate of the 8 blocks before it. The output depends only on the samples, not on how they are split into `feed()` calls, and is the same on every platform (`test_demod.cpp` pins a hash). No allocation after construction.

**Scored against the truth and readsb** ([`scripts/check-demod.sh`](scripts/check-demod.sh), CI job `iq`; same files, same scorer, readsb v3.16.21 with `--no-fix`):

| Scenario (10 s unless noted, 2.4 MS/s) | Messages decoded: ours / readsb | Clean at 15–30 dB SNR | False decodes |
|---|---|---|---|
| 150 aircraft, `cu8` | 6,332 / 6,257 (+1.2%) | 99.7% / 98.8% | 16 / 10, never a DF17 |
| Weak signals (noise figure 22 dB) | 1,136 / 1,029 (**+10%**) | 98.8% / 96.5% | 11 / 5 |
| 40 ppm, DC offset, 5% / 3° IQ imbalance (6 s) | 2,462 / 2,391 | 99.5% / 97.8% | 12 / 10 |
| Strong multipath (\|Γ\| 0.7, reflector 3 dB down; 6 s) | 1,364 / 1,134 (+20%) | 55% / 46% | 129 / 88, of which 114 are echoes |
| 2.0 MS/s (readsb's file input is 2.4 MS/s only; 3 s) | 819 | 92% (blind phase, below) | 1 |
| 12 MS/s (3 s) | 921 | | 5 |

Every start is within 0.14 samples (0.06 µs) of the truth, against readsb's 0.54. readsb's own counts vary by about 0.5% from run to run on the same file; ours do not. The false decodes are, like readsb's, DF11 replies with a wrong bit in the last seven parity bits (they read as an interrogator code; the address is right). Our extra ones come from decoding more weak replies. On its own pulse model (`test_demod.cpp`, 400 frames, noise σ 0.01 per component), the half-bit comparison decodes 64% at amplitude 0.06 and the sequence detector 88%; at 0.10 both are at 99–100%.

At **2.0 MS/s** a half-bit is exactly one sample, and a frame that starts at a sample centre puts half of every pulse into each of two samples, so the half-bit comparison cannot read it (dump1090's 2 MS/s demodulator has the same blind phase). The sequence detector reads such frames when the signal is strong; in noise some are lost. Use 2.4 MS/s.

**Speed** (one thread, M2 laptop; main and this version run alternately, 5 rounds, medians). Two rounds of work, each with byte-identical output: first removing waste found by profiling each stage, then four-wide SIMD (NEON on ARM64, SSE2 on x86-64, through the GCC/Clang vector extension):

| Recording | First version | After profiling | Now (SIMD) | Real time |
|---|---|---|---|---|
| 150 aircraft, 40 s, 2.4 MS/s | 24.4 ns/sample | 8.68 ns | 6.07 ns | 69× |
| Weak signals (noise figure 22 dB) | 21.7 | 7.66 | 5.23 | 80× |
| 200 aircraft, 6 MS/s | 18.7 | 7.62 | 6.02 | 28× |
| 400 aircraft, 2.0 MS/s, strong reflector | 75.1 | 31.2 | 21.1 | 24× |

The first column was measured on a busier laptop than the other two, so it overstates the first round's gain; the last two columns were measured side by side: the SIMD round is 1.27–1.48 times faster.

First round:
- the scan jumps over positions the gate closed (`memchr` over the gate bytes) instead of visiting all of them;
- the gate's window sums have a fixed width, and the larger of each pulse's two windows is taken once per position;
- noise-floor windows are summed straight from the buffer;
- the sequence detector runs one Viterbi pass for both frame lengths (the path through the first 56 bits does not depend on the rest);
- the timing climb reuses evaluations.

SIMD round:
- chips four at a time: the four products of each chip in one vector, then a 4×4 transpose so that each lane adds them in the scalar order, ((p0 + p1) + p2) + p3; the four pulses of a phase in one call, the twelve quiet chips in three; a frame's bit margin is added up only once it passes;
- the sequence detector computes a sample's four squared errors (one per pulse pattern) as one vector with a vector square root, looks up which error each hypothesis takes in a table instead of a switch, and keeps the survivor paths as bit registers instead of tracing back through the decisions;
- noise windows four at a time (transposed, each lane summing its 16 samples in order), and the lowest quarter by insertion instead of `nth_element` plus `sort` (that selection alone was 0.8 µs per 1,024-sample block);
- the timing template takes a fixed number of samples per chip, adding +0.0 where the old loop stopped (the template is never −0.0, so that changes nothing): the old variable trip count was mispredicted about once per chip.

Every output field is byte-identical to the version before each round: the four recordings above and 29 more runs (2.0, 3, 8 and 12 MS/s, cs16, `--fix`, `--no-sequence`, 1 to 8 phases, every magnitude method) compared byte for byte against the previous build; two pinned hashes in `test_demod.cpp` that print every float field in `%a` (exact bits) plus the counts of the decisions on the way, one of them on weak signals where the sequence detector decides many frames; the pins match on macOS Clang and on Linux GCC 14 and Clang 18, ARM64 and x86-64. Independent reviews checked each claim. The first round's review caught two changes that were not identical (one moved the noise floor's rounding, one lost a real overlapping reply); both were reverted. The second round's review compared every frame field, in `%a`, on 15 more rates from 1.9 to 100 MS/s, random block sizes, gaps and truncated files, and found no difference. It also checked 2.6e10 cases of the fixed per-chip sample count. Its one finding was NaN passed straight to `feed_magnitudes()`, which now counts as silence as it does in `feed()`. Rejected because they were slower: keeping the template's shared sample in a register, gathering the detector's errors ahead of the recursion, a fixed number of sample slots per bit, and stopping the quiet sum early (their branches mispredict).

readsb decodes the same 40 s recording at 5.35 ns per sample on this laptop (process time in Docker's Linux VM), so it is now about 1.13 times faster than us (1.8 before the SIMD round), and we decode more frames. Where the time goes now: preamble checks 26%, sequence detector 27%, timing 18%, bit reading and checks 10%, noise floor 10%, gate 6%, magnitudes 4%. Magnitudes alone: exact `sqrt` 0.42 ns per sample (vectorized), the classic 64K-entry, 128 KB lookup table 0.78 ns (it does not fit in L1, §5.2), alpha-max-beta-min 0.47 ns (`bench_demod.cpp`).

The plan's target of 10 cycles per sample (about 3 ns) is not met: what remains is serial by construction (the detector's path costs add sample by sample, in order) or would change the output (a cheaper second-stage test). Shortcuts measured and rejected because they lost frames: reading only the clearest phase in a frame's tail (−8 of 25,035) and skipping the detector there (one lost overlapping reply).

```bash
./build/release/tools/adsb-demod /tmp/scene.sigmf-meta --beast /tmp/scene.beast      # SigMF: format and rate from the metadata
./build/release/tools/adsb-iqsynth --data - --no-meta | ./build/release/tools/adsb-demod - --format cu8 --raw -
python3 scripts/iq-score.py /tmp/scene.sigmf-meta /tmp/scene.beast
```

Not done: real RTL-SDR recordings (§5.3; the generator is the only input so far), live `librtlsdr` ingest (§5.1; the IQ path into `adsb-live` below takes files and stdin), SIMD preamble and sequence detector, Mode A/C replies, and suppression of multipath echoes (readsb does not suppress them either).

### IQ into the live pipeline (README_UPDATES.md §5.1)

`adsb-live --iq FILE` runs the whole pipeline on raw samples, with our demodulator in place of readsb: IQ reader → block ring → demodulator → decode + track state → feed. [`IqSource`](include/adsb/ingest/iq_source.hpp) gives the ingest thread the same records the Beast source does.

- **Reader thread:** reads blocks of `--iq-block` samples (default 16,384 = 6.8 ms at 2.4 MS/s, an RTL-SDR's 32 KB USB transfer) from a SigMF recording, a raw file or stdin. With `--iq-speed X` (default 1) a block is released when its last sample would have arrived, so a recording behaves like a receiver. `--iq-speed 0` reads as fast as the demodulator goes.
- **Block ring:** `--iq-buffers` slots (default 16). When it is full, the block is dropped and counted (the overload table above).
- **Frame times:** a frame's `rx_ns` is the host time its last sample arrived, from a lower envelope of block arrival times against the sample index (jitter only delays a block), never decreasing. The latency it starts covers the wait for the rest of the block, the demodulator, decode and track state. Unpaced file input has no arrival clock: its frames are timed on the sample clock from the first block, so event time (track expiry, duplicate detection) is the recording's.
- **A live receiver on stdin:** `rtl_sdr -f 1090e6 -s 2.4e6 - | adsb-live --iq - --iq-format cu8 --iq-speed 0 --iq-drop` (untested here: no SDR on hand). `--iq-drop` makes unpaced input drop full blocks like a receiver, and times frames by arrival.

[`scripts/check-iq-live.sh`](scripts/check-iq-live.sh) (CI job `iq`, 9 checks), 100 aircraft on the M2 laptop:

| Check | Result |
|---|---|
| Unpaced, from a file and from stdin | the recording holds exactly the 3,545 frames `adsb-demod` finds, in order |
| Real time, published to a feed subscriber | no block lost; the subscriber's book equals the publisher's |
| Last sample → track state, 1.7 / 6.8 / 27.3 ms blocks | p50 1.2 / 5.2 / 20 ms, p99 2.1 / 9.2 / 37 ms: about half a block plus scheduling (the plan's USB transfer-size trade-off, simulated) |
| 100× real time, 4 buffers | 77% of the blocks dropped and counted; the rest decode and the pipeline carries on |

The block-size row is the trade-off §5.1 plans to measure on a real RTL-SDR: smaller transfers cut the latency almost in proportion, at the cost of more wake-ups. On the laptop the reader sometimes wakes milliseconds late (`max lateness` in the report), which shows in the p99.

### Transponder frequency study (README_UPDATES.md §22.5)

How far is each aircraft's transmitter from 1090 MHz, and how much does it drift? The standard allows ±1 MHz. The pieces:

- **Per message** ([`CarrierEstimator`](include/adsb/demod/carrier.hpp)): once a message is decoded, its pulse pattern is known. Each sample gets a weight (the share of it that pulses cover) and a time (the middle of the covered part). The estimate maximizes |Σ w z e^{−j2πft}|:
  1. coarse peak of a zero-padded FFT over the whole band;
  2. a grid of 1/(4T) over two FFT bins;
  3. weighted least squares of each sample's remaining phase against time.

  The fit's residuals give a standard error (over the effective number of samples). `coherence` (1 = one clean carrier across the message) flags collisions and transmitters whose phase jumps between pulses. ADC-clipped samples are left out; `adsb-carrier` marks them before it removes the DC offset, which would otherwise move them below the clip level. A fit that moves more than two grid steps from the grid's peak is rejected.
- **Per recording** ([`adsb-carrier`](tools/adsb_carrier.cpp)): demodulates, estimates every frame, runs the frames through the track engine, and computes each message's Doppler from the aircraft's decoded position (moved forward from its last accepted fix, changed or not, with its velocity) and velocity. Output is one CSV row per frame; the centre frequency comes from the SigMF metadata. About 12–23× real time at 2.4 MS/s on one core.
- **The study** ([`carrier-study.py`](scripts/carrier-study.py)):
  - **Receiver error:** given (`--rx-ppm`, a GPS-disciplined or calibrated receiver), or estimated as the value that makes the mean aircraft offset zero (good only to about 100 kHz / √N, and printed with its standard error).
  - **Receiver drift** (`--rx-drift BIN_S`, a receiver warming up): alternating least squares with each aircraft's own offset and drift, and one receiver value per time bin. It is measured from t = 0, so a given or estimated ppm is the LO error at the start. The population's mean drift is taken as zero. Without receiver drift the option only adds noise.
  - **Per aircraft:** offset, drift with its standard error, residual spread, and Allan deviation at 1–50 s, of the offsets and of the residuals after the drift fit.
  - **Groups:** by emitter category and by type (`--types`: a CSV, or `ads/aircraft-types-by-hex.json` for the FAA class of US registrations).
  - **Ground truth:** `--truth` scores it all against the generator.

[`scripts/check-carrier.sh`](scripts/check-carrier.sh) (CI job `iq`, 11 checks), generator scenarios:

| Scenario | Result |
|---|---|
| 60 aircraft, 60 s, 2.4 MS/s cu8, receiver +12 ppm known | per message: error p50 23 Hz, p95 117 Hz; reported sigma right (\|err/σ\| p50 0.75; 0.67 = exact); Doppler error p50 0.4 Hz, p95 2.5 Hz. Per aircraft: offset error p50 6 Hz, p90 11 Hz; drift error p50 0.08 Hz/s |
| The same, receiver ppm estimated from the aircraft | +27.8 ± 12.4 ppm (true +12): the aircraft's own scatter (σ ≈ 100 kHz) limits it; differences between aircraft keep their 6 Hz |
| Offsets up to ±1 MHz (σ 500 kHz) at 2.4 MS/s | measured (largest 0.9 MHz), same per-aircraft precision |
| LO drifting 0.02 ppm/s (22 Hz/s) for 120 s | without `--rx-drift`, every drift is 22 Hz/s off; with `--rx-drift 10`: receiver drift 0.01996 ppm/s found, aircraft drifts within 0.04 Hz/s, offsets within 3 Hz of each other plus a common 10 Hz from extrapolating the receiver drift back to t = 0 |
| Transmitters within 1 kHz, receiver +50 ppm drifting 0.005 ppm/s | the ppm at t = 0 estimated from the aircraft within 0.3 ppm (a sign or timing error in the ppm would show here) |
| 6 MS/s cs16, a reflector 12 dB down, DC offset | multipath is the largest error: 92 Hz per message, and not in the reported sigma; aircraft offsets still within 15 Hz (their geometry changes) |

Findings that matter for the real study:
- **Multipath dominates:** the echo has its own Doppler and overlaps other bits. A site with few reflectors (or a directional antenna) matters more than SNR.
- **Phase coherence is assumed.** A transmitter whose phase jumps between pulses (old cavity-oscillator transponders) has coherence < 0.5 in the unit test. Such aircraft are listed as "mostly incoherent", not measured.
- **The receiver's crystal limits absolute offsets:** an RTL-SDR at 1 ppm is 1.09 kHz off. Aircraft-to-aircraft differences and drifts don't need a reference. Absolute offsets better than ~10 ppm / √(N/60) need a GPS-disciplined receiver or a known reference signal.
- `--lo-drift` on `adsb-iqsynth` (new) drifts only the LO. The sample clock's drift would shift every sample position, and the truth assumes they are fixed.

On a real recording (an RTL-SDR at 2.4 MS/s; keep the gain below clipping):

```bash
rtl_sdr -f 1090000000 -s 2400000 -g 30 -n 1440000000 capture.cu8     # 10 minutes
core/build/release/tools/adsb-carrier capture.cu8 --format cu8 --rate 2.4e6 --receiver LAT,LON,ALT_M --out messages.csv
python3 core/scripts/carrier-study.py messages.csv --rx-drift 10 --types ads/aircraft-types-by-hex.json --aircraft-out aircraft.csv
```

### A/B line arbitration (Workstream B, README_UPDATES.md §6)

Two receivers at one site hear the same transmissions but lose different ones. Like a feed handler with an exchange's A and B feeds, the [`Arbiter`](include/adsb/ingest/arbiter.hpp) forwards the first copy of every frame and drops the later ones, so the output is the union of the lines, and each frame is as early as the faster line delivered it. ADS-B has no sequence numbers: a frame is its payload (56 or 112 bits) inside a window. Identical payloads legitimately repeat no sooner than about 400 ms (velocity, identification, DF11), and a dropped repeat would carry nothing new anyway.

- **Hot path:** expire entries older than the window, one open-addressing lookup (exact compare of the 14-byte payload and its length, backward-shift deletion), insert or count. Fixed memory, no allocation, no waiting for the slower line. Up to four lines. If the table fills before the window ends the oldest entry leaves early (`evicted early` in the report).
- **Event time:** decisions use the records' `rx_ns`, so replaying a recording repeats them. In `adsb-live` the state thread merges the two lines' queues by `rx_ns` (one frame of lookahead per line; a frame that still arrives out of order has its time raised, `merge` in the report) and records every copy of both lines, so `adsb-replay --arb-window-us W` on that recording makes exactly the live run's decisions.
- **Mode A/C** (2-byte replies, the same for every aircraft on one code) pass through unarbitrated.
- **Measurements (§6.3):** per line: frames received, first copies, wins (frames several lines received where this line was first), late copies, frames only this line received; the win margin (later copy minus first, per winning line); and the relative radio clock of lines 0 and 1, a linear fit of `radio_ts(1) − radio_ts(0)` against line 0's counter (both 48-bit values unwrapped). Only DF17/18 pairs feed it: replies to interrogations can repeat byte for byte inside the window, and a copy of one reply paired with a copy of another would be a sample milliseconds off. A pair whose difference is more than 100 µs (plus 200 ppm of drift) from the last accepted pair's is rejected, and 64 rejections in a row restart the fit (a receiver restarted its counter). At one site the true arrival difference is about 0, so the slope is the two receivers' crystal offset in ppm (groundwork for multilateration, §15.4).
- **Windows:** the library and `adsb-replay` default to 5 ms. `adsb-live` picks 60 ms for Beast lines (readsb sends its output in batches of up to 50 ms, so two readsb's copies of a frame can arrive tens of ms apart) and twice the block duration for IQ lines (a block's frames are handed over together; with 16,384-sample blocks a 5 ms window missed 54 of 924 copies in the check below).
- **Tools:** `adsb-live --b-host H --b-port P` (line B from a second readsb) or `--iq-b PATH` (a second IQ input, with `--iq`); `adsb-replay --arbitrate [--arb-window-us N]`; `adsb-synth --lines 2 [--outage L:FROM:TO] [--line-b-delay US]`; `adsb-beast-serve --line N` serves one line of a recording.

```bash
# Two receivers, line B unplugged from 20 to 40 s: arbitrated replay
./build/release/tools/adsb-synth -o /tmp/ab.adsbrec --aircraft 300 --duration 60 --lines 2 --outage 1:20:40
./build/release/tools/adsb-replay /tmp/ab.adsbrec --arbitrate

# Live from two readsb instances (one per RTL-SDR)
./build/release/tools/adsb-live --port 30005 --b-port 30006 --record /tmp/ab-live.adsbrec
```

[`scripts/check-ab.sh`](scripts/check-ab.sh) (CI job `ab`, 22 checks) and `test_arbiter.cpp` (17 tests), on the laptop:

| Check | Result |
|---|---|
| Against the generator's ground truth (200 aircraft, 2 lines, 1% corrupted) | every distinct received frame forwarded exactly once, always its earliest copy; only-A / only-B / both and each line's wins equal the truth exactly; line A (250 µs faster) wins > 80% by a median of ~250 µs |
| Brute-force model, 200,000 random frames on 3 lines, tables of 16 / 64 / 1,024 entries | the same decision for every frame and the same counters, including early evictions |
| Relative radio clock | +18.000 ppm, the generator's line B drift, with a 34 ns residual (its tick rounding: 83 ns / √6); across a 48-bit counter wrap in a unit test |
| Replay, 200 aircraft × 30 s | output 25% larger than either line alone; the engine sees no duplicate; inline and two-thread replay decide alike and give the same output hash |
| Outages (one line unplugged for 15 s, or each line for 10 s in turn) | everything else unchanged (the generator draws the same numbers); no aircraft expires; no silence longer than one line's own longest |
| Live, two Beast servers into `adsb-live`, line B stopped after 2.5 s | both lines connect; replaying the run's recording repeats its arbitration line for line; line A carries on alone, all 200 tracks stay, nothing dropped; +18.000 ppm from the Beast timestamps |
| Live, two IQ receivers with independent noise (NF 20 dB), our demodulator on each line | the arbiter matches exactly the frames both demodulators decoded (744; counted apart from it, by payload and sample time) and forwards their union (1,138, 16% more than the better line's 979); line B's +20 ppm sample clock recovered as +20.000 ppm |

Cost: about **25 ns per frame** (`bench_arbiter.cpp`, M2), the same for real traffic and with 1,000× compressed time (4,500 frames inside the window); the decode + state stage after it is ~120 ns.

Review and mutation testing. An independent review found three bugs, all fixed with tests:
- the index stored the low 32 bits of the entry counter, so after 2^32 frames (about an hour at 1 M frames/s) one entry equalled the empty marker and corrupted the table; it now stores ring positions;
- two IQ lines ending together could both leave stopping `adsb-live` to the other;
- the IQ window was computed before the SigMF sample rate was read.

It also prompted the reply-pair filter and outlier rejection in the clock fit, unwrapping the difference (an offset near 2^47), one table size for live and replay, refusing line B on line A's own source, and guards in the check script. Mutation testing: the reviewer's 12 planted bugs and 11 more against the fixes were all caught. Making the survivors of the first round fail exposed a precision bug: the fit lost ~0.3 ppm when the difference was near 2^47 ticks (a double resolves 1/32 tick there), so it now works relative to its first point.

Engine output with and without the arbiter: the engine's own per-track duplicate filter already removed cross-line copies, so the output is nearly the same. In the 300-aircraft example 1 of 42,813 messages differs: line A delivered a DF4 reply before the track was confirmed (rejected), line B's copy came after (accepted); with the arbiter B's copy is a duplicate, as with one receiver.

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
| Decoder | Published vectors from *The 1090 MHz Riddle* and pyModeS (also target state TC 29 and Comm-B 1,0 1,7 2,0 4,0 5,0 6,0, each checked for a clean CRC or a plausible recovered address); synthetic frames for every DF, TC 31 versions 0–2 airborne and surface, and each Comm-B format rule (status bits, reserved bits, ranges, an MB field both 5,0 and 6,0 accept is left undecoded); single-bit repair fixes every bit outside the format field, never a format bit (a DF19 read as DF17), never a two-bit error (all 5,671 pairs), never an Address/Parity frame; in the track engine a repaired frame only updates a track a clean frame confirmed (1 M random DF17 frames, some of which "repair", create no track), 20,000 repaired frames of a confirmed aircraft are all recovered, and a repaired copy of a frame seen clean on another line is a duplicate; random valid frames stay in physical ranges; `fuzz_decoder` covers repair and the new fields. An independent review (against pyModeS and readsb) found that repaired frames could create phantom aircraft (readsb refuses that too), that TC 31's heading-reference bit was inverted, that a 6,0 rule comparing baro and inertial vertical rates made 5% of real 6,0 replies with transient rates read as 5,0, that duplicates were hashed before repair, and that a repaired frame lost its syndrome; all fixed with tests. Mutation testing: 40 of 40 planted bugs caught |
| Encoders | Reproduce the published example frames exactly; round trips through the decoder over the full altitude, squawk, velocity, position, and surface-speed ranges |
| CPR | Riddle and pyModeS examples; NL table against the closed form; 100k random global/local round trips; zone-boundary rejection |
| Altitude | Exhaustive Gillham Gray-code property check over all 1,280 codes |
| SPSC ring / pipeline | Edge cases; two-thread stress with torn-slot detection; pipeline replay under ThreadSanitizer; GenMC model with mutants and `adsb-ring-stress` on x86 and ARM (`spec/memory-model/`) |
| Histogram | Exhaustive bucket tiling; error bound; percentiles against exact sort; every integer percentile of 1..100 |
| Beast | Every chunking of a stream heavy with `0x1A`; garbage, unknown types, truncation, reset |
| Recording | Round trip; empty file; corruption detected by CRC; truncated file recovers every complete record; bad records rejected |
| Track store | 300k-operation randomized differential test against `std::unordered_map` (including clustered keys); probe lengths at full load |
| Track engine | Unit tests for each rule. **End to end against ground truth:** 60 aircraft, 2 lines, 1% corrupted frames. Zero phantom tracks, every reported position within 12 m of the true transmitted position, callsigns/squawks/categories correct, duplicates filtered, every track expires. **Feed v1 fields** (`adsb-synth --avionics`: 300 aircraft, ADS-B versions 0–2, 39 type-code/supplement rows from a NIC table written apart from the decoder's): every track's final `nic` and `adsb_version` equal the simulated ones, headings within 2.5° and airspeeds within 1.5 kt of the truth with the right TAS/IAS and true/magnetic flags; unit tests for the pair minimum, local fixes against the track, supplement expiry, rebroadcast and reserved-version statuses, NIC-C's class-code guard, and each `update_mask` bit. An independent review against readsb's source found that `bench/legacy/analyze.py` still parsed 64-byte updates (the benchmark's "after" numbers would have come out empty), and five differences from readsb (local fixes did not keep the track's lower NIC, supplements never expired, bit 8 read as NIC-B for TIS-B/ADS-R where it is IMF, reserved versions read as version 2, NIC-C read without its class-code guard); all fixed with tests. Mutation testing: 28 of 28 planted bugs caught (a 29th was equivalent; the dead check it exposed was removed) |
| Determinism | Same output hash across repeated runs, paced and max speed, inline and pipeline, optimized and naive builds (`scripts/check-determinism.sh`, CI) |
| Feed protocol | Golden packet decoded and re-encoded identically in C++, Python, and JavaScript; every truncation rejected; generated code checked for staleness in CI |
| Gap recovery | TLA+ model checked by TLC ([`spec/`](../spec/)); deliberately broken variant fails |
| Feed subscriber | A unit test for each rule (gap, overlap, duplicate, heartbeat tail, timeout retry, chunked requests, NAK, stale NAK, older/newer snapshot, late join, restart, stale session, end of session, window overflow, unknown types). A **randomized simulation** of 85 runs (loss, duplication, reordering, long delays, outages, lost requests and replies, slow and failed snapshots, a 256-message ring, restarts, late joins) checks the TLA+ invariants at every step: messages applied in order exactly once, and the book always equal to the publisher's as of `expected − 1`. It also checks that every recovery path actually ran. |
| Deterministic simulation | `adsb-dst` (above): 200,000 quick and 5,000 deep worlds pass (laptop, about 1,500 and 70 seeds/s on 8 threads); in CI 40,000 new seeds per push under GCC and Clang (equal sweep hashes) and a nightly 40-minute sweep (`.github/workflows/dst-nightly.yml`). `test_dst.cpp`: a sweep with non-vacuity counts (NAKs, snapshots, session changes, stale packets, window overflows, truncated replies, queue overflows, stalls), same seed same run, and two sabotaged worlds (a corrupted retransmit, a snapshot one message ahead) that the checks must catch. It found four subscriber defects, all fixed in C++ and in the gateway's JS port, with unit tests in both: (1) a late packet of the old session around a restart could leave a subscriber live on the dead session forever (a change of session is now confirmed by the snapshot server, in the background: the subscriber stays live, so a busy server, which takes 16 snapshot clients, delays nothing); (2) an asynchronous snapshot fetch asked for before a session change, answered by a server that died since, could do the same (an answer is used only if no change of session happened since the request: the subscriber counts session changes, because after two quick restarts the stale answer can carry the very session the subscriber has moved to — found by a CI sweep, seed 40013996); (3) a reply truncated by the server's cap waited a full retransmit timeout before the rest was asked for (a short final reply packet now completes the request); (4) requests larger than the window were thrown away when reply packets reordered (now clamped to the window). Not reachable at simulated rates: a full publisher log during a recovery stall (modeled: the publisher blocks), since deep worlds publish at most 100 messages/ms. Mutation testing: see the PR. |
| Feed over sockets | Publisher → UDP → subscriber with the recovery servers on their own thread: no loss, batched with feed and reply loss, a 64-message ring (snapshot fallback), a late joiner, and multicast to two subscribers. The final books are byte-identical. `scripts/check-feed.sh` repeats this with the real tools and also checks that the publisher's book equals a plain deterministic replay's. |
| AF_XDP reception | `test_xdp_receiver.cpp`: a small eBPF interpreter runs the exact program on crafted frames, faulting on any read outside the packet or of an unset register: the feed is redirected to the frame's queue, with addresses and ports whose bytes are above 0x7F; ARP, IPv6, IP options, fragments (DF alone is not one), TCP, other ports and addresses pass; no frame shorter than the headers is read past its end; and for every header byte the program and `udp_payload` agree. Live over the veth pair (CI job `xdp`, as an unprivileged user with file capabilities): unicast and multicast arrive intact and in order through the XSK and none through the kernel socket; other ports still reach the kernel; closing detaches the program; a second receiver on the device is refused with a clear message; `SubscriberClient` rebuilds the publisher's book from AF_XDP alone; `check-feed.sh` with `FEED_RX=xdp` covers live recovery with injected drops on the AF_XDP path, snapshot recovery, and a publisher restart. Mutation testing: 18/18 planted bugs caught (program bounds, version/IHL, fragment mask, 64-bit address compare, port offset, fallback action, map key, UDP check; parser lengths and padding; fill ring and RX consumer; map size; client integration, kernel counter, port 0, fault injection). An independent review found no correctness bug; its points (header checksum, MTU check, snapshot recovery untested over AF_XDP, a leaked fd on the verifier-log retry, CLI checks) are fixed |
| Synthetic IQ | `test_iq.cpp` (34 tests): portable math against libm; link budget (free-space loss, dipole pattern, horizon and diffraction); two-ray bounce against a brute-force search for the reflection point on the sphere (path difference within 1% at 10–200 km, divergence factor), none beyond the horizon; echo delay, its own Doppler, its legs' diffraction; Doppler sign and size; LO error; input validation; the waveform carries the right bits at 2.0, 2.4, 8 and 12 MS/s in every format after quantization; pulse positions and amplitude; the carrier frequency in the samples equals the truth (within 0.01 Hz at 12 MS/s); noise power and independent noise per chunk; DC offset and IQ imbalance sample by sample; collisions flagged (also spans that just touch) and the scene equal to the sum of its messages; a message straddling a chunk boundary equal to the same message inside a chunk; the recording extended for late messages; a pinned cross-platform hash. Mutation testing: 28 of 29 planted bugs caught without the hash test (the survivor, a redundant horizon guard, is equivalent). An independent review found 3 bugs (unvalidated inputs could give NaN samples and invalid JSON; a tail of 1e14 overflowed the sample count) and 6 weaknesses (annotations past the end of the data; a flat-ground bounce on a curved-Earth model; the delayed-path phase from the transmit frequency instead of the LO; echoes ignoring the horizon; the Beast parser misreading frames after a status frame; vacuous tests): all fixed, with tests. End to end: readsb decodes the files (above) |
| Demodulator | `test_demod.cpp` (26 tests), mostly on its own ideal PPM renderer (independent of the generator): every rate from 2.4 to 100 MS/s and every fractional start decodes exactly with the start within 20 ns; the 2.0 MS/s blind phase (the half-bit comparison fails, the sequence detector reads it); all four formats agree; DF11 interrogator codes 1–79 accepted, 80 and up rejected; Address/Parity frames only from addresses a DF11/17/18 showed, and not after the TTL; the single-bit fix only when asked, never on the format bits (a DF19 with bit 3 wrong reads as a DF17), with the corrected address; noise, silence, a carrier, clipping, a square wave and NaN/inf give no frames, and noise passes the gate at about 1%; decode rate against amplitude, with and without the sequence detector; output independent of the feed() split and of the block size, also under noise that changes within a block; flush at the end; a reply starting under the previous one's tail; back-to-back frames; Beast round trip; generator scenarios against their truth (≥ 97% clean at 15–30 dB, no false DF17, starts within half a sample); a pinned hash, equal on macOS Clang, Linux GCC 14 and Clang 18. `fuzz_demod`: arbitrary samples in every format up to 100 MS/s; every frame passes its CRC or carries an address seen before; the same frames with a 1,024- or 65,536-sample block size. An independent review found 3 bugs: the noise-estimate ring held 16 blocks while one feed completes 64 (the floor came from later samples, so the output depended on the block size); the timing search wrote before its buffer at 60–100 MS/s; `--fix` reported, and whitelisted, the uncorrected address. All are fixed and tested, along with NaN input, the output array's size and the CLI's error handling. Mutation testing: 25 of 31 planted bugs caught without the hash test. Of the 6 survivors, 4 change nothing observable: the end-of-stream fit check (the zero padding already fails such frames), the phase tie-break, the preference for a clean DF11 reading, and the output array's size (frames cannot come closer than 64 µs unless they overlap). The other 2 are the quiet-part check, which only saves time, and the timing search's margin, which no test drives to its reach; an explicit bounds check now backs that margin. End to end: `scripts/check-demod.sh` (above) |
| IQ ingest | `test_iq_source.cpp` (13 tests): the block ring fills, refuses, and frees in order, and across two threads delivers whole blocks in order with every miss counted as a drop; `Demodulator::skip` keeps frames after a gap on the original sample clock and creates no frame, `skip(0)` changes nothing; `IqSource` unpaced gives exactly the demodulator's frames for 4K/16K/64K blocks, also from a pipe written in odd-sized pieces, timed on the sample clock; paced, frame times follow the paced clock, no frame reaches the sink before its last sample is due, and few times need raising; a slow consumer loses whole blocks, counted, and what is decoded is still real; stop ends a run (also during a long pacing sleep); an invalid configuration allocates nothing. The timing tests pace at a fraction of what the build's demodulator manages, so they also hold in debug and sanitizer builds. An independent review found 5 defects: timing tests that failed under sanitizers; unpaced input timed by read speed (98% of frame times raised, 1,759 real frames then dropped as duplicates); a pacing sleep that ignored stop; sizes allocated before validation; frames lost on `skip(0)`. All are fixed with tests, and TSan and ASan found no races or memory errors. Mutation testing: 16 of 19 planted bugs caught; the survivors are a defensive clamp, a redundant end-of-input break, and stale noise slots, which give the same minimum. End to end: `scripts/check-iq-live.sh` (above) |
| A/B arbitration | `test_arbiter.cpp` (17 tests) and `scripts/check-ab.sh` (22 checks), see *A/B line arbitration* above: ground truth from the generator, a brute-force model, outages, live Beast and IQ lines |
| Fuzzing | `fuzz_beast`, `fuzz_decoder`, `fuzz_packet`, `fuzz_feed` (snapshot stream, subscriber datagrams, retransmit requests), `fuzz_demod` (sample streams) with invariant checks (round trips, no out-of-bounds reads, tracker never moves backwards, replies carry exactly the logged messages) |
| Test quality | Mutation testing: planted bugs in each module are caught by the suite |

## Limits and next steps

- **Decoder gaps:** TC 29 subtype 0 (DO-260A), DF19 and Comm-B registers other than 1,0 1,7 2,0 3,0 4,0 5,0 6,0 are not decoded; Comm-B values are not yet used by the track engine (an inferred register can be wrong, and 5,0 and 6,0 are often both plausible, which pyModeS settles with the aircraft's ADS-B speed). Single-bit repair (`--fix` on `adsb-replay` and `adsb-live`, off by default) closes most of the gap to readsb: on the 1,000-aircraft comparison (`bench/legacy/compare-positions.sh`) the new decoder goes from 0.9964 to 0.9994 of readsb's positions; what remains is mostly readsb repeating unchanged positions, and frames with a wrong format bit, which are never repaired. The GNSS-height unit question (feet vs metres, see `decoder.hpp`) awaits the readsb differential diff on real traffic.
- **Track engine:** `nic` follows readsb's rules (feed spec §3), so it inherits readsb's readings of versions 0 and 1 (e.g. bit 8 counts as NIC-B for TC 16 in every version); NACp, SIL and NUCp are decoded (TC 31) but not carried. Heading and airspeed come only from ADS-B velocity subtypes 3/4, not from Comm-B (5,0 / 6,0). The NIC supplements expire 60 s after the last TC 31 that set them, as in readsb; the version is kept for the track's lifetime, also as in readsb.
- **Overload:** defined and tested for `adsb-live` (above), and the conflated feed (§10.2) is built. The SDR → IQ ring boundary is defined and tested for IQ files paced like a receiver (`adsb-live --iq`); a live SDR source is not built yet and an overload test of the replay/publish tools. `adsb-live`'s state thread spins when idle (one core), by design.
- **Feed transports:** kernel UDP, io_uring (publisher side only; subscribers receive with `recv()` or AF_XDP), and the shared-memory ring. AF_XDP is measured in copy mode only (zero-copy needs a supporting NIC); its reception binds one device queue (a multi-queue NIC needs flow steering to that queue, or feed frames on other queues go through the kernel socket) and takes the device's feed frames, so one AF_XDP subscriber per device; hardware timestamps (§9.4) are not built. io_uring sends are not zero-copy (`SEND_ZC` costs more than a copy below about 1.5 KB). The shm ring is not yet backed by explicit huge pages (Linux gets transparent huge pages when the kernel allows them for shmem). Its slot protocol (both the fence form and the TSan form) is model-checked with GenMC (`spec/memory-model/`); after a lap, readers resume with `resume_after_lap()`, since `next_seq()` can lag the lap just seen. IPv4 only. The subscriber's snapshot fetch blocks its thread (datagrams queue in the kernel meanwhile). The snapshot server copies the whole book on the recovery thread, which delays retransmits by that long (about 20 µs per 1,000 tracks). The retransmit server answers any requester (LAN only: no rate limit against spoofed requests). If the recovery thread stalls for long enough to fill its 65,536-entry log, the publisher waits rather than lose retransmit state. A reused session id cannot be detected by subscribers, so the tools generate a unique one per start.
- **Demodulator:** tested on synthetic IQ only (no real recordings yet); not SIMD. It runs in `adsb-live` on IQ files and stdin (`--iq`); a live `librtlsdr` source is not built. See *Demodulator* and *IQ into the live pipeline* above.
- **A/B arbitration:** tested on simulated lines and on two synthetic IQ receivers; no second RTL-SDR yet, so no real two-receiver numbers. The relative clock fit is linear (offset and rate); a real crystal also wanders with temperature.
- The WebSocket gateway (Node) is in [`gateway/`](../gateway/README.md).
