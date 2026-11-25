# WebSocket gateway

Subscribes to the track feed (multicast UDP, with full gap recovery), keeps the track table in memory, and pushes it to browsers over WebSockets (README_UPDATES.md §9.6, §10). It also serves the map (`ads/`) from memory instead of Postgres: the 2-second database poll becomes a push.

```
adsb-publish / adsb-live --publish ──multicast──▶ adsb-gateway ──WebSocket──▶ browsers (ads/ map)
        ▲ retransmit (UDP) / snapshot (TCP) ◀────────┘      └──HTTP──▶ /api/aircraft, trails, statistics (Fall 2025 API)
```

Node 22, no dependencies (`npm install` is not needed).

## Quick start

```bash
cd core && cmake --preset release && cmake --build --preset release && cd ..
core/build/release/tools/adsb-synth -o /tmp/demo.adsbrec --aircraft 150 --duration 900 --receiver 41.88,-87.63
node gateway/bin/adsb-gateway.mjs --feed 127.0.0.1:41900 &
core/build/release/tools/adsb-publish /tmp/demo.adsbrec --feed 127.0.0.1:41900 --bind 127.0.0.1 --speed 1 --busy-wait
# open http://127.0.0.1:8080/
```

The default feed is the multicast group `239.19.0.1:41900` (feed_spec.md §5). The example above uses unicast loopback, because loopback multicast on macOS stalls for about 26 ms at times (Linux is fine). `--receiver 41.88,-87.63` puts the synthetic traffic inside the map's default view of Chicago.

| Tool | What it does |
|---|---|
| `bin/adsb-gateway.mjs` | The gateway. `--help` lists options: feed, retransmit and snapshot addresses, `--listen` (default `127.0.0.1:8080`), `--flush-ms`, `--high-water`, fault injection (`--drop-rate` ...), `--until-end`. The last line printed on exit is the book digest. |
| `bin/adsb-ws-client.mjs` | A browser stand-in. It keeps the table exactly as the map does, measures publish → received latency, and prints the table's digest. `--slow-ms N` makes it a slow client; `--window N` sets its flow-control window (0 = off). |

Endpoints: `/` serves the map, `/gateway/ws` is the WebSocket, `/gateway/stats` returns subscriber, fan-out and latency counters as JSON, and `/gateway/digest` returns the book digest.

## How it works

**Feed side.** `src/feed.mjs` is a port of the C++ `SubscriberCore` (`core/src/feed/subscriber.cpp`), rule for rule. It handles in-order apply, buffering ahead of `expected`, retransmit requests with timeouts and chunking, the NAK → TCP snapshot fallback, sessions (restart, stale packets, 8 retired sessions), end of session, and the forged-sequence bound. `src/feed_client.mjs` adds sockets. Its snapshot fetch is asynchronous, so the feed keeps being buffered while a snapshot is in flight. The book keeps raw wire bytes, so its digest equals the C++ `FeedBook::digest()` of the same state.

**Fan-out.** Each client has a set of ICAO addresses that changed since its last frame. A flush sends the current bytes of each (or a delete). After every frame, the client's table therefore equals the gateway's book at the frame's `as_of_seq`, however far the client fell behind. A slow client receives less data, but each frame brings it fully up to date. Its pending state is bounded by the number of aircraft, never by the message rate, and it never delays other clients.

**Flow control.** A client that opts in keeps at most `window` frames unacknowledged. While it is behind, its changes are conflated into the next frame. Without this, a slow browser's backlog hides in kernel socket buffers, which on loopback autotune to megabytes. A client stalling 3 ms per frame then fell more than 18 s behind. With a window of 8 it stays about 20 ms behind (8 frames × its own 3 ms). The map (`ads/app.js`) and `adsb-ws-client` both use window 8 and acknowledge every 4 frames. Clients without flow control are still served: the gateway writes a frame only once the previous one is in the kernel (`--high-water 0`). A client stuck for 10 s (`--stuck-ms`) is disconnected.

**The map.** `ads/` is served as it is. Both pages, the 2D map (`index.html`, `app.js`) and the 3D globe (`3d.html`, `cesium-app.js`), take the live feed through `ads/live-feed.js` (until November 2026 the 2D map had a separate shim that the gateway added to the page, and the 3D globe only polled). When a page can load `/gateway/gateway_protocol.mjs`, it:
- keeps a live table from the WebSocket;
- redraws from that table when frames arrive, at most every 100 ms (`?render_ms=N`), on an animation frame, and stops polling `/api/aircraft` while in sync;
- shows markers at their latest position instead of easing over 2 s (`?animate=1` restores the easing);
- acknowledges frames for flow control;
- reconnects with backoff, with the 2-second poll covering the time out of sync (back in sync about 1 s after a gateway restart).

The 3D globe keeps its 2 s poll of `/api/aircraft/enriched` while in sync, but only for what the feed does not carry: the aircraft type (helicopter models), trails, and the probability cones (computed by the gateway, below). It adds a trail point every 2 s (so trails still span about 100 s) and moves the camera in follow mode at most every 1.5 s. On the laptop, an update of 150 aircraft takes about 4 ms of the page's time (`window.__gateway.renderTimeMs`; the globe's entities are in `window.__globe`). `gateway/test/live_feed.test.mjs` runs `live-feed.js` as a page would, against a real gateway.

Served by the Fall 2025 `server.js`, there is no protocol module, so the page polls as before.

`/api/aircraft`, `/api/aircraft/:hex/trail` (from an in-memory ring of 200 positions per aircraft), `/api/statistics` and `/api/health` are answered from memory in the Fall 2025 JSON shapes. Aircraft objects also carry the feed v1 fields the Fall 2025 API did not have: `heading` (degrees) with `heading_ref` (`true`/`magnetic`), `airspeed` (kt) with `airspeed_type` (`tas`/`ias`) and `air_data_source` (`adsb`/`comm_b`), `nic`, `adsb_version`, `nac_p`, `sil` with `sil_type` (`per_hour`/`per_sample`) and `nuc_p` (null when unknown). `/api/aircraft/enriched` adds each aircraft's `trail` and `probabilityCone`, and `/api/collisions/kalman` returns the Kalman collision warnings, both in the Fall 2025 shapes (see *Probability cones and collisions*). Trail points carry `event_ms`, the feed's event time of the position (TrackUpdate `event_ts_ns`, whole ms).

### Probability cones and collisions

`src/prediction.mjs` is the Fall 2025 model (`ads/kalman-filter.js`, `ads/probability-cone.js`) without its mathjs dependency: a constant-velocity Kalman filter in ECEF trained on the aircraft's history, predicted forward to "now", then 20 steps over the horizon with 95% (chi-squared 7.815) or 99% radii; collisions are the first step at which two cones meet within the vertical separation rule (1,000 ft, 2,000 ft above FL290). Its matrices act on each axis separately, so the port runs three 2-state filters, with each entry computed from the same terms in the same order as the 6x6 products. `test/prediction.test.mjs` runs the original modules (with a small matrix library standing in for mathjs) on the same inputs and requires identical numbers, cones and collision lists alike.

What differs from `ads/server.js` is where the inputs come from:

- **History:** the in-memory trail (200 positions, about 100 s at ADS-B rates) instead of Postgres. Enriched uses the trail points of the last 120 minutes, collisions those of the last 10 minutes and only aircraft with an altitude, updated in the last 2 minutes and with at least 2 positions in the window, as the server's queries did.
- **Time:** event time. "Now" is the newest `event_ts_ns` in the book and history timestamps are the trail points' `event_ms`; the server used `Date.now()` and database timestamps. Cones of a replayed recording therefore start from where the aircraft were at the replay's time, not from the wall clock.
- **Parameters:** `cones=false`, `trails=false`, `prediction_time` (s, default 300) and `confidence` (default 0.95) on enriched; `time_horizon` and `confidence` on collisions, parsed as the server did. `window` is ignored (the book holds the current aircraft).
- **Cost:** nothing runs on the feed path. A request computes the cones it needs and keeps each for `coneCacheMs` (2 s; the server kept them 3 s), keyed by the trail points used, so the collision check reuses the enriched cones when the windows hold the same points. Collision results are kept for `collisionCacheMs` (2 s). Cones are computed in slices of `predictionSliceMs` (4 ms), yielding to the event loop in between. Pairs whose cones' bounding boxes do not overlap are skipped (same result as checking every pair). `/gateway/stats` reports `prediction` counters and the last computation times. On the laptop (loaded, load average 6–17), with 1,000 aircraft and 200-point trails: a cone costs about 40 µs, all 1,000 about 40 ms; the collision check over cached cones about 10 ms. The enriched response itself takes about 115 ms to serialize even with `cones=false` (200,000 trail points); the cones add about 20 ms of JSON.

Static files: only known file types are served, never dotfiles (`ads/.env` holds the database password) or `node_modules`, and nothing outside the map directory.

## Protocol (gateway → browser)

Defined in `public/gateway_protocol.mjs`, which the gateway and the browser share.

Each binary WebSocket message is one frame. All integers are little-endian, like the feed.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `kind` | u8 | `'S'`: snapshot, replace the whole table. `'D'`: delta, apply on top. |
| 1 | `version` | u8 | 0 |
| 2 | `flags` | u16 | bit 0 `STALE`: the gateway is recovering from feed loss. bit 1 `ENDED`: the publisher's session ended. |
| 4 | `count` | u32 | Messages that follow |
| 8 | `as_of_seq` | u64 | After this frame the table equals the publisher's book after feed message `as_of_seq` |
| 16 | messages | | `count` × (u16 length + feed message): `TrackUpdate` `'U'` or `TrackDelete` `'D'`, exactly as on the feed (feed_spec.md §3) |

The first frame on a connection is a snapshot. A new snapshot follows a publisher restart or a gateway snapshot recovery.

Messages from the gateway, text JSON: `{"type":"status", "session", "as_of_seq", "mode", "stale", "ended", "tracks", "clients", "time_ms"}` on connect and every second.

Messages from the client:
- **Hello**, text: `{"type":"hello","window":N}` with 1 ≤ N ≤ 64. Turns on flow control.
- **Ack**, binary, 5 bytes: `'A'` + u32 = binary frames processed since the connection opened (cumulative). Send one at least every ⌈N/2⌉ frames.
- An ack for frames never sent, a bad window, or non-JSON text closes the connection (1008, 1008, 1007). Unknown JSON types are ignored.

The server accepts client messages up to 64 KiB.

## Measured (MacBook, laptop numbers)

| Path | Result | Conditions |
|---|---|---|
| publish → gateway book | p50 40 µs | Unicast loopback, publisher at 10× real time, 200 aircraft |
| publish → WebSocket client (`adsb-ws-client`, Node) | p50 95 µs, p99 about 2 ms | Same run |
| Slow client (stalls 3 ms per frame), no flow control | over 1 s behind (p50); max 18.6 s | 300 aircraft, 15× real time |
| Slow client, window 8 | p50 19–25 ms | Same run |
| Browser (the map, 150 aircraft) | markers at the latest position (0 m off); one full render 8 ms | Built-in browser pane |
| Fall 2025 system, for comparison | 1.1–1.6 s frame → map | `bench/results/legacy-baseline/` |

**Caveats.**
- These are laptop numbers, and the machine was under background load (load average 5–12). The same run varied by 10× in median between repetitions, so treat the table as orders of magnitude.
- `check-gateway.sh` runs three gateways, four clients and a 15× publisher at once. There, every client's median is tens of milliseconds, because the machine is saturated.
- Latency is measured against the publisher's clock (`publish_ts_ns`, CLOCK_MONOTONIC_RAW). The gateway calibrates against it through `core/tools/adsb-clock`, to within about 10 µs. Without the helper, latencies are labelled "uncalibrated".
- Browser render-to-pixels time is not measured. A hidden browser tab throttles animation frames, and renders follow.
- Rerun on the benchmark host for the report.

## Tests

```bash
cd gateway && node --test test/*.test.mjs          # about 80 tests, about 10 s
gateway/scripts/check-gateway.sh core/build/release # integration against the C++ publisher (26 checks)
```

- `test/subscriber.test.mjs`: every rule of the C++ subscriber's scenario tests, plus the randomized simulation. The simulation covers loss, duplication, reordering, outages, restarts and late joins, and checks in-order apply and PrefixOfPublisher at every step.
- `test/feed.test.mjs`: framing, the golden packet, the book and its digest, and snapshot parsing under every chunking and malformation.
- `test/websocket.test.mjs`: RFC 6455 framing, control frames, fragmentation, limits, close codes, and interoperability with Node's built-in WebSocket client.
- `test/gateway.test.mjs`: the whole gateway on real sockets against a JS publisher with retransmit and snapshot servers. Covers loss, the ring overrun → snapshot path, a late join, a restart, end of session, a slow client (bounded and conflated), stuck clients, flow-control rules, the REST endpoints, and static-file safety.
- `test/prediction.test.mjs`: cones and collision lists identical to the Fall 2025 modules on every path (history, unsorted history, history too old, one fix, no altitude; several horizons and confidence levels; 120 aircraft, every pair); a straight flight's cone points along its track and widens; converging aircraft are reported, diverging or vertically separated ones are not; the gateway's endpoints from its trails in event time, the caches, `cones=false`, and the 2-minute activity rule.
- `test/fanout.test.mjs`: fan-out and flow-control rules with stub connections, so timing-dependent cases (a socket that stays full, a feed that goes quiet) are deterministic.
- `scripts/check-gateway.sh`: the real C++ publisher over multicast, with 1% loss, a 2,048-message ring, a gateway losing bursts of 4,000 datagrams, a late-joining gateway, a slow client, and a restart. Every gateway's book and every client's table must equal the publisher's book byte for byte. The restart run must also equal a plain deterministic replay. It runs in CI (job `gateway`).

## Limits

- **Network exposure:** no TLS and no authentication. The gateway listens on 127.0.0.1 by default. Put it behind nginx (as `ads/nginx-adsb.conf` does for the Fall 2025 server) to expose it.
- **One process:** Node is single-threaded, so feed processing and fan-out share one core. At real-world rates (hundreds to a few thousand messages a second) this is far from the limit.
- **Map features:** Postgres-backed history is not provided. Trails last as long as the gateway's memory, 200 points per aircraft, so cones and collisions are trained on at most that much history.
- **Map tiles:** the map uses Esri's dark gray canvas and its labels layer, which need no API key (the Fall 2025 CARTO tiles now answer "API key required"). They are fetched from Esri by each browser, so the map needs internet access.
- **Feed choice:** the gateway works on either feed. The plan (§9.6) has it subscribe to the conflated feed: publish one with `--conflate 239.19.0.3:41910` and start the gateway with `--feed 239.19.0.3:41910 --retransmit 127.0.0.1:41911 --snapshot 127.0.0.1:41912`. It conflates per client in either case. On ADS-B traffic, the 10 Hz conflated feed carries 96% of the full feed's updates, so it caps per-aircraft rates more than it saves bandwidth.

An independent review found two flow-control bugs, both fixed with regression tests in `test/fanout.test.mjs`:
- a client skipped once was not retried, so it could stall on a quiet feed;
- an ack did not re-send a pending change of flags only, such as ENDED.

Mutation testing planted 31 bugs across the subscriber, fan-out, flow control, WebSocket framing and static serving. The tests catch 30. The survivor removes the directory-containment check, which is a second layer: any escape needs a `..` segment, and the dotfile rule already refuses those.
