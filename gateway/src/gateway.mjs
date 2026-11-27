// The WebSocket gateway (README_UPDATES.md §10): subscribes to the track feed
// with full gap recovery, keeps the book in memory, and pushes it to browsers
// over WebSockets. It also serves the map (ads/; app.js takes the live feed
// from /gateway/ws when it can load /gateway/gateway_protocol.mjs), and
// answers the map's REST endpoints (/api/aircraft, trails, statistics) from
// memory instead of Postgres.
//
// Fan-out is conflated per client. Each client has a set of ICAO addresses
// that changed since its last frame. A flush sends the current bytes of each
// (or a delete), so after every frame the client's table equals the gateway's
// book at `as_of_seq`, however much the client fell behind. A client whose
// socket has more than `highWater` bytes unsent is skipped (its set keeps
// growing, bounded by the number of tracks) and is disconnected if it stays
// stuck for `stuckMs`. One slow browser therefore costs memory proportional to
// the airspace, never to the message rate, and never delays other clients.

import { readFileSync, statSync } from 'node:fs';
import http from 'node:http';
import { dirname, extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

import { FrameFlags, FrameKind, encodeFrameHeader, toAircraft } from '../public/gateway_protocol.mjs';
import { ClockSync, LatencyHistogram, hrNs } from './clock.mjs';
import { ApplyResult, Mode } from './feed.mjs';
import { FeedClient } from './feed_client.mjs';
import { ProbabilityCone, detectCollisionsFromCones, formatCollision } from './prediction.mjs';
import { acceptUpgrade } from './websocket.mjs';

const here = dirname(fileURLToPath(import.meta.url));
export const repoRoot = resolve(here, '..', '..');
const publicDir = resolve(here, '..', 'public');

const kPositionBit = 1; // UpdateMask.kPosition
const kDeleteEntryBytes = 16;

const contentTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.glb': 'model/gltf-binary',
};

export const defaultGatewayOptions = Object.freeze({
  listen: { host: '127.0.0.1', port: 8080 },
  mapDir: join(repoRoot, 'ads'),
  flushMs: 0, // 0: flush as soon as the current burst of datagrams is processed
  highWater: 0, // skip a client while more than this is unsent (0: until its last frame is taken by the kernel)
  stuckMs: 10_000,
  maxClients: 256,
  statusMs: 1000,
  trailPoints: 200,
  clockHelper: null,
  coneCacheMs: 2000, // reuse an aircraft's probability cone this long (the Fall 2025 server: 3 s)
  collisionCacheMs: 2000, // reuse the /api/collisions/kalman result this long
  predictionSliceMs: 4, // compute cones in slices of about this long, yielding to the feed in between
});

// The Fall 2025 server's history windows (ads/server.js), in event time.
const kConeHistoryMs = 120 * 60_000; // enriched: trails of the last 120 minutes
const kCollisionHistoryMs = 10 * 60_000; // collisions: positions of the last 10 minutes
const kCollisionActiveMs = 2 * 60_000; // collisions: aircraft seen in the last 2 minutes

const u16le = (n) => {
  const b = Buffer.alloc(2);
  b.writeUInt16LE(n, 0);
  return b;
};
const kLenUpdate = u16le(72); // TrackUpdate::kWireSize
const kLen16 = u16le(kDeleteEntryBytes);

function deleteMessage(icao) {
  const d = Buffer.alloc(kDeleteEntryBytes);
  d[0] = 0x44; // 'D'
  d[2] = 2; // reason: evicted (the gateway's own; it does not know why)
  d.writeUInt32LE(icao, 4);
  return d;
}

export class Gateway {
  constructor(opts = {}) {
    this.opts = { ...defaultGatewayOptions, ...opts };
    this.clients = new Set();
    this.seen = new Map(); // icao -> { first, last } wall ms (for the Fall 2025 API)
    this.trails = new Map(); // icao -> [{lat, lon, altitude, speed, track, seen, event_ms}]
    // Probability cones, computed per REST request (never per feed message):
    // key -> {at (wall ms), cone}; and the last collision results.
    this.coneCache = new Map();
    this.collisionCache = new Map();
    this.predictionStats = { cones_computed: 0, cone_cache_hits: 0, collision_runs: 0, last_cones_ms: 0, last_collisions_ms: 0 };
    this.clock = new ClockSync(this.opts.clockHelper);
    this.lat = {
      feed_to_gateway: new LatencyHistogram(), // publish_ts -> applied to the book
      feed_to_ws: new LatencyHistogram(), // publish_ts -> written to a client socket
    };
    this.counters = {
      frames: 0,
      snapshot_frames: 0,
      bytes: 0,
      messages_sent: 0,
      conflated: 0, // updates replaced by a newer one before a client got them
      skipped_flushes: 0, // client over highWater at flush time
      flow_holds: 0, // client had `window` frames unacknowledged at flush time
      acks: 0,
      clients_total: 0,
      clients_dropped_stuck: 0,
      clients_rejected: 0,
    };
    this.flushScheduled = false;
    this.aircraftTypes = this.loadAircraftTypes();
    this.feed = new FeedClient({
      ...this.opts.feedOptions,
      handler: {
        onMessage: (seq, msg, r) => this.onFeedMessage(seq, msg, r),
        onReset: () => this.onBookReplaced(true),
        onSnapshotLoaded: () => this.onBookReplaced(false),
      },
    });
  }

  get core() {
    return this.feed.core;
  }

  get book() {
    return this.feed.core.book;
  }

  loadAircraftTypes() {
    try {
      return JSON.parse(readFileSync(join(this.opts.mapDir, 'aircraft-types-by-hex.json'), 'utf8'));
    } catch {
      return {};
    }
  }

  async start() {
    await this.clock.start();
    await this.feed.open();
    // Flags (STALE, ENDED) can change without a message being applied (a
    // heartbeat reveals a gap, the end-of-session marker arrives): tell clients.
    this.lastFlags = this.frameFlags();
    this.feed.onInput = () => {
      const f = this.frameFlags();
      if (f !== this.lastFlags) {
        this.lastFlags = f;
        this.scheduleFlush();
      }
    };
    this.server = http.createServer((req, res) => this.onHttp(req, res));
    this.server.on('upgrade', (req, socket, head) => this.onUpgrade(req, socket, head));
    await new Promise((res, rej) => {
      this.server.once('error', rej);
      this.server.listen(this.opts.listen.port, this.opts.listen.host, () => {
        this.server.off('error', rej);
        res();
      });
    });
    this.statusTimer = setInterval(() => this.broadcastStatus(), this.opts.statusMs);
    this.statusTimer.unref();
    return this;
  }

  address() {
    return this.server.address();
  }

  async stop() {
    clearInterval(this.statusTimer);
    clearTimeout(this.retryTimer);
    for (const c of this.clients) c.ws.close(1001, 'gateway stopping');
    this.feed.close();
    this.clock.stop();
    await new Promise((res) => this.server.close(() => res()));
  }

  // ---- Feed side ----------------------------------------------------------

  onFeedMessage(_seq, msg, r) {
    if (r !== ApplyResult.kUpdate && r !== ApplyResult.kDelete) return;
    const icao = msg.readUInt32LE(4);
    for (const c of this.clients) {
      if (c.needSnapshot) continue;
      if (c.dirty.has(icao)) this.counters.conflated++;
      else c.dirty.add(icao);
    }
    const nowMs = Date.now();
    if (r === ApplyResult.kUpdate) {
      const pub = Number(msg.readBigUInt64LE(56));
      if (pub !== 0) this.lat.feed_to_gateway.record(this.clock.now() - pub);
      const s = this.seen.get(icao);
      if (s) s.last = nowMs;
      else this.seen.set(icao, { first: nowMs, last: nowMs });
      if (this.opts.trailPoints > 0 && msg.readUInt16LE(2) & kPositionBit) this.addTrailPoint(icao, msg, nowMs);
    } else {
      this.seen.delete(icao);
      this.trails.delete(icao);
    }
    this.scheduleFlush();
  }

  addTrailPoint(icao, msg, nowMs) {
    const a = toAircraft(msg);
    if (a.lat === null || a.lon === null) return;
    let t = this.trails.get(icao);
    if (!t) this.trails.set(icao, (t = []));
    // event_ms: the feed's event time of the frame that moved it (TrackUpdate
    // event_ts_ns), whole ms: the time base of the probability cones.
    const eventMs = Number(msg.readBigUInt64LE(8) / 1_000_000n);
    t.push({ lat: a.lat, lon: a.lon, altitude: a.altitude, speed: a.speed, track: a.track, seen: Math.floor(nowMs / 1000), event_ms: eventMs });
    if (t.length > this.opts.trailPoints) t.splice(0, t.length - this.opts.trailPoints);
  }

  // A new session cleared the book, or a snapshot replaced it.
  onBookReplaced(cleared) {
    const nowMs = Date.now();
    this.coneCache.clear();
    this.collisionCache.clear();
    if (cleared) {
      this.seen.clear();
      this.trails.clear();
    } else {
      for (const icao of this.seen.keys()) if (!this.book.tracks.has(icao)) this.seen.delete(icao);
      for (const icao of this.trails.keys()) if (!this.book.tracks.has(icao)) this.trails.delete(icao);
      for (const icao of this.book.tracks.keys()) {
        const s = this.seen.get(icao);
        if (s) s.last = nowMs;
        else this.seen.set(icao, { first: nowMs, last: nowMs });
      }
    }
    for (const c of this.clients) {
      c.needSnapshot = true;
      c.dirty.clear();
    }
    this.scheduleFlush();
  }

  // ---- Fan-out ------------------------------------------------------------

  frameFlags() {
    let f = 0;
    if (this.core.mode === Mode.kSnapshot || this.core.gapOpen) f |= FrameFlags.kStale;
    if (this.core.ended) f |= FrameFlags.kEnded;
    return f;
  }

  asOfSeq() {
    return this.core.expected - 1;
  }

  scheduleFlush() {
    if (this.flushScheduled) return;
    this.flushScheduled = true;
    const run = () => {
      this.flushScheduled = false;
      this.flush();
    };
    if (this.opts.flushMs > 0) setTimeout(run, this.opts.flushMs);
    else setImmediate(run);
  }

  flush() {
    const now = Date.now();
    const flags = this.frameFlags();
    const asOf = this.asOfSeq();
    let blocked = false;
    for (const c of this.clients) {
      if (!c.ws.open) continue;
      if (!c.needSnapshot && c.dirty.size === 0 && c.sentFlags === flags && c.sentSeq === asOf) continue;
      const overBuffer = c.ws.bufferedBytes > this.opts.highWater;
      const noCredit = c.window > 0 && c.sentFrames - c.ackedFrames >= c.window;
      if (overBuffer || noCredit) {
        if (overBuffer) this.counters.skipped_flushes++;
        else this.counters.flow_holds++;
        if (c.stuckSince === 0) c.stuckSince = now;
        if (now - c.stuckSince > this.opts.stuckMs) {
          this.counters.clients_dropped_stuck++;
          c.ws.terminate(1008, 'client too slow');
        } else blocked = true; // including the first skip: retry below
        continue;
      }
      c.stuckSince = 0;
      this.sendTo(c, flags, asOf);
    }
    // A skipped client is retried when its socket drains, and also on a short
    // timer: 'drain' only fires if a write saw a full buffer, and the kernel
    // can absorb the backlog (loopback buffers grow) without that happening
    // after the skip.
    if (blocked && !this.retryTimer) {
      this.retryTimer = setTimeout(() => {
        this.retryTimer = null;
        this.scheduleFlush();
      }, 20);
    }
  }

  sendTo(c, flags, asOf) {
    const parts = [null];
    let count = 0;
    let bytes = 0;
    const tracks = this.book.tracks;
    const snapshot = c.needSnapshot;
    if (snapshot) {
      for (const u of tracks.values()) {
        parts.push(kLenUpdate, u);
        count++;
        bytes += 2 + 72; // u16 length + TrackUpdate
      }
      c.needSnapshot = false;
      c.dirty.clear();
      this.counters.snapshot_frames++;
    } else {
      const nowPub = this.clock.now();
      for (const icao of c.dirty) {
        const u = tracks.get(icao);
        if (u) {
          parts.push(kLenUpdate, u);
          bytes += 2 + 72; // u16 length + TrackUpdate
          const pub = Number(u.readBigUInt64LE(56));
          if (pub !== 0) this.lat.feed_to_ws.record(nowPub - pub);
        } else {
          parts.push(kLen16, deleteMessage(icao));
          bytes += 18;
        }
        count++;
      }
      c.dirty.clear();
    }
    parts[0] = encodeFrameHeader(snapshot ? FrameKind.kSnapshot : FrameKind.kDelta, flags, count, asOf);
    c.ws.sendBinary(parts);
    c.sentFrames++;
    c.sentFlags = flags;
    c.sentSeq = asOf;
    this.counters.frames++;
    this.counters.messages_sent += count;
    this.counters.bytes += bytes + 16;
  }

  // Flow control (gateway/README.md, Protocol). A client opts in with the
  // text message {"type":"hello","window":N} (1 <= N <= 64), then sends a
  // 5-byte binary ack, 'A' + u32 LE = binary frames it has finished
  // processing since the connection opened (cumulative). The gateway keeps at
  // most N frames unacknowledged; changes meanwhile are conflated into the
  // next frame. Without this, a slow browser's backlog hides in kernel socket
  // buffers (megabytes on loopback) and it falls seconds behind.
  onClientMessage(c, data, binary) {
    if (binary) {
      if (data.length !== 5 || data[0] !== 0x41) {
        c.ws.close(1003, 'unknown binary message');
        return;
      }
      const n = data.readUInt32LE(1);
      if (n < c.ackedFrames || n > c.sentFrames) {
        c.ws.close(1008, 'ack out of range');
        return;
      }
      c.ackedFrames = n;
      this.counters.acks++;
      // Anything held for credit, including a change of flags (ENDED, STALE)
      // or of as_of_seq with no track changed.
      if (c.needSnapshot || c.dirty.size > 0 || c.sentFlags !== this.frameFlags() || c.sentSeq !== this.asOfSeq()) {
        this.scheduleFlush();
      }
      return;
    }
    let m;
    try {
      m = JSON.parse(data.toString());
    } catch {
      c.ws.close(1007, 'expected JSON');
      return;
    }
    if (m && m.type === 'hello') {
      const w = m.window;
      if (!Number.isInteger(w) || w < 1 || w > 64) {
        c.ws.close(1008, 'window must be an integer in [1, 64]');
        return;
      }
      c.window = w;
    }
    // Other message types are ignored (forward compatibility).
  }

  status() {
    return {
      type: 'status',
      session: this.core.hasSession ? this.core.session : null,
      as_of_seq: this.asOfSeq(),
      mode: this.core.mode,
      stale: (this.frameFlags() & FrameFlags.kStale) !== 0,
      ended: this.core.ended,
      tracks: this.book.size,
      clients: this.clients.size,
      time_ms: Date.now(),
    };
  }

  broadcastStatus() {
    if (this.clients.size === 0) return;
    const s = JSON.stringify(this.status());
    for (const c of this.clients) if (c.ws.open && c.ws.bufferedBytes <= this.opts.highWater) c.ws.sendText(s);
  }

  onUpgrade(req, socket, head) {
    const path = (req.url || '').split('?')[0];
    if (path !== '/gateway/ws') {
      socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
      return;
    }
    if (this.clients.size >= this.opts.maxClients) {
      this.counters.clients_rejected++;
      socket.end('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n');
      return;
    }
    const ws = acceptUpgrade(req, socket, head);
    if (!ws) return;
    // window > 0: flow control negotiated by the client's hello (see onClientMessage).
    const c = { ws, dirty: new Set(), needSnapshot: true, stuckSince: 0, sentFlags: -1, sentSeq: -1, window: 0, sentFrames: 0, ackedFrames: 0 };
    this.clients.add(c);
    this.counters.clients_total++;
    ws.on('close', () => this.clients.delete(c));
    ws.on('drain', () => this.scheduleFlush());
    ws.on('message', (data, binary) => this.onClientMessage(c, data, binary));
    ws.sendText(JSON.stringify(this.status()));
    this.scheduleFlush();
  }

  // ---- HTTP ---------------------------------------------------------------

  stats() {
    const st = this.core.stats;
    return {
      status: this.status(),
      subscriber: { ...st, expected: this.core.expected, known_next: this.core.knownNext, feed_dropped: this.feed.loss.dropped },
      fanout: { ...this.counters },
      prediction: { ...this.predictionStats, cached_cones: this.coneCache.size },
      latency_us: {
        feed_to_gateway: this.lat.feed_to_gateway.summary(),
        feed_to_ws: this.lat.feed_to_ws.summary(),
      },
      clock: this.clock.describe(),
      last_snapshot_error: this.feed.lastSnapshotError || null,
    };
  }

  digest() {
    return {
      session: this.core.hasSession ? this.core.session : null,
      as_of_seq: this.asOfSeq(),
      ended: this.core.ended,
      tracks: this.book.size,
      digest: this.book.hexDigest(),
    };
  }

  aircraftList() {
    const out = [];
    for (const [icao, u] of this.book.tracks) {
      const s = this.seen.get(icao);
      const a = toAircraft(u, s ? Math.floor(s.last / 1000) : null);
      if (a.lat === null || a.lon === null) continue;
      a.type = this.aircraftTypes[a.hex] ?? null;
      out.push(a);
    }
    return out;
  }

  statistics() {
    const list = this.aircraftList();
    const alts = list.map((a) => a.altitude).filter((x) => x !== null);
    const speeds = list.map((a) => a.speed).filter((x) => x !== null);
    const avg = (xs) => (xs.length ? xs.reduce((s, x) => s + x, 0) / xs.length : 0);
    const ranges = [
      ['0-10k', 10000],
      ['10-20k', 20000],
      ['20-30k', 30000],
      ['30-40k', 40000],
      ['40k+', Infinity],
    ];
    const dist = ranges
      .map(([range, hi], i) => ({
        range,
        count: alts.filter((x) => x < hi && (i === 0 || x >= ranges[i - 1][1])).length,
      }))
      .filter((d) => d.count > 0);
    const durations = list
      .map((a) => {
        const s = this.seen.get(Number.parseInt(a.hex, 16));
        return {
          hex: a.hex,
          flight: a.flight,
          duration_seconds: s ? Math.round((s.last - s.first) / 1000) : 0,
          first_seen: s ? Math.floor(s.first / 1000) : null,
          last_seen: s ? Math.floor(s.last / 1000) : null,
        };
      })
      .sort((x, y) => y.duration_seconds - x.duration_seconds);
    return {
      summary: {
        total_flights: list.length,
        avg_speed: Math.round(avg(speeds) * 10) / 10,
        avg_altitude: Math.round(avg(alts)),
        min_altitude: alts.length ? Math.min(...alts) : 0,
        max_altitude: alts.length ? Math.max(...alts) : 0,
        min_speed: speeds.length ? Math.min(...speeds) : 0,
        max_speed: speeds.length ? Math.max(...speeds) : 0,
      },
      altitude_distribution: dist,
      flight_durations: durations,
      timestamp: Date.now(),
    };
  }

  // ---- Probability cones and collisions (ads/server.js, from memory) -------
  //
  // The Fall 2025 server ran the Kalman filter over Postgres history against
  // Date.now(). Here the history is the in-memory trail and "now" is the
  // feed's event time: the newest event_ts_ns in the book. Everything is
  // computed when a REST request asks for it, and cones are reused for
  // coneCacheMs, so the feed -> book -> WebSocket path never runs it. A
  // request computes its cones in slices of predictionSliceMs and yields to
  // the event loop in between, so datagrams and WebSocket frames are not held
  // up for the whole computation (1,000 aircraft: about 40 ms).

  async forEachSliced(items, fn) {
    let t = performance.now();
    for (let i = 0; i < items.length; i++) {
      fn(items[i]);
      if ((i & 15) === 15 && performance.now() - t > this.opts.predictionSliceMs) {
        await new Promise((r) => setImmediate(r));
        t = performance.now();
      }
    }
  }

  eventNowMs() {
    let max = 0n;
    for (const u of this.book.tracks.values()) {
      const t = u.readBigUInt64LE(8);
      if (t > max) max = t;
    }
    return Number(max / 1_000_000n);
  }

  // History in the original's shape ({lat, lon, altitude, timestamp}), points
  // with event time >= sinceMs.
  history(icao, sinceMs) {
    const out = [];
    for (const p of this.trails.get(icao) ?? []) {
      if (p.event_ms >= sinceMs) out.push({ lat: p.lat, lon: p.lon, altitude: p.altitude, timestamp: p.event_ms });
    }
    return out;
  }

  sweepCaches(wallMs) {
    for (const [k, e] of this.coneCache) if (wallMs - e.at >= this.opts.coneCacheMs) this.coneCache.delete(k);
    for (const [k, e] of this.collisionCache) if (wallMs - e.at >= this.opts.collisionCacheMs) this.collisionCache.delete(k);
  }

  // A ProbabilityCone for `a` (an aircraftList() object) from its trail
  // points of the last `historyMs`, or null when the original would not
  // compute one (fewer than 2 positions and, unless `requireHistory`, no speed
  // and track) or it fails. Cached by the history it is computed from, so the
  // enriched and collision endpoints share a cone whenever their windows hold
  // the same points (with 200-point trails, nearly always).
  coneFor(a, horizon, confidence, nowMs, historyMs, requireHistory) {
    const icao = Number.parseInt(a.hex, 16);
    const since = nowMs - historyMs;
    let n = 0;
    let first = 0;
    let last = 0;
    for (const p of this.trails.get(icao) ?? []) {
      if (p.event_ms < since) continue;
      if (n === 0) first = p.event_ms;
      last = p.event_ms;
      n++;
    }
    if (n < 2 && requireHistory) return null;
    const key = `${icao}:${horizon}:${confidence}:${n}:${first}:${last}`;
    const wallMs = Date.now();
    const hit = this.coneCache.get(key);
    if (hit && wallMs - hit.at < this.opts.coneCacheMs) {
      this.predictionStats.cone_cache_hits++;
      return hit.cone;
    }
    const history = this.history(icao, since);
    let cone = null;
    if (history.length >= 2 || (a.speed && a.track)) {
      try {
        cone = new ProbabilityCone(a, history, horizon, confidence, nowMs);
      } catch {
        cone = null;
      }
      this.predictionStats.cones_computed++;
    }
    if (this.opts.coneCacheMs > 0) this.coneCache.set(key, { at: wallMs, cone });
    return cone;
  }

  // /api/aircraft/enriched?trails=&cones=&prediction_time=&confidence=
  // (window is accepted and ignored: the book holds the current aircraft).
  async enriched(params) {
    const includeTrails = params.get('trails') !== 'false';
    const includeCones = params.get('cones') !== 'false';
    const predictionTime = Number.parseInt(params.get('prediction_time')) || 300;
    const confidenceLevel = Number.parseFloat(params.get('confidence')) || 0.95;
    const aircraft = this.aircraftList();
    const t0 = performance.now();
    if (includeCones) {
      this.sweepCaches(Date.now());
      const nowMs = this.eventNowMs();
      await this.forEachSliced(aircraft, (a) => {
        const cone = this.coneFor(a, predictionTime, confidenceLevel, nowMs, kConeHistoryMs, false);
        a.probabilityCone = cone ? cone.getPredictionPoints() : null;
      });
      this.predictionStats.last_cones_ms = performance.now() - t0;
    }
    if (includeTrails) for (const a of aircraft) a.trail = this.trails.get(Number.parseInt(a.hex, 16)) ?? [];
    return {
      now: Date.now(),
      aircraft,
      timeWindow: 0,
      source: 'gateway',
      stats: {
        total: aircraft.length,
        withTrails: aircraft.filter((a) => a.trail && a.trail.length > 0).length,
        withCones: aircraft.filter((a) => a.probabilityCone).length,
      },
    };
  }

  // /api/collisions/kalman?time_horizon=&confidence=: aircraft with a
  // position and altitude, updated in the last 2 minutes, with at least 2
  // positions in the last 10 minutes (all in event time).
  //
  // Aircraft on the ground take no part (a change from the Fall 2025 logic,
  // which checked every pair): like TCAS, which does not treat aircraft on
  // the ground as threats. On real traffic almost every warning was between
  // aircraft parked at Midway, or an arrival passing over them. Counted in
  // on_ground_excluded.
  async collisions(params) {
    const timeHorizon = Number.parseInt(params.get('time_horizon')) || 300;
    const confidence = Number.parseFloat(params.get('confidence')) || 0.95;
    const wallMs = Date.now();
    this.sweepCaches(wallMs);
    const key = `${timeHorizon}:${confidence}`;
    const hit = this.collisionCache.get(key);
    if (hit && wallMs - hit.at < this.opts.collisionCacheMs) return { ...hit.body, timestamp: wallMs, cached: true };
    const t0 = performance.now();
    const nowMs = this.eventNowMs();
    let onGround = 0;
    const active = this.aircraftList().filter((a) => {
      if (a.altitude === null) return false;
      if (a.on_ground) {
        onGround++;
        return false;
      }
      const u = this.book.tracks.get(Number.parseInt(a.hex, 16));
      return Number(u.readBigUInt64LE(8) / 1_000_000n) >= nowMs - kCollisionActiveMs;
    });
    const valid = [];
    const cones = [];
    await this.forEachSliced(active, (a) => {
      const cone = this.coneFor(a, timeHorizon, confidence, nowMs, kCollisionHistoryMs, true);
      if (!cone) return; // fewer than 2 positions in the window
      valid.push(a);
      cones.push(cone);
    });
    let body;
    if (valid.length < 2) {
      body = {
        collisions: [],
        message: 'Not enough aircraft with sufficient position history',
        aircraft_count: valid.length,
        on_ground_excluded: onGround,
        time_horizon: timeHorizon,
        confidence_level: confidence,
      };
    } else {
      const byHex = new Map(valid.map((a) => [a.hex, a]));
      const current = (a) => (a ? { lat: a.lat.toFixed(4), lon: a.lon.toFixed(4), alt: Math.round(a.altitude) } : null);
      const collisions = detectCollisionsFromCones(cones).map((c) => {
        const f = formatCollision(c);
        const ac1 = byHex.get(c.aircraft1);
        const ac2 = byHex.get(c.aircraft2);
        f.currentPosition1 = current(ac1);
        f.currentPosition2 = current(ac2);
        f.flight1 = ac1?.flight || null;
        f.flight2 = ac2?.flight || null;
        return f;
      });
      body = { collisions, aircraft_count: valid.length, on_ground_excluded: onGround, time_horizon: timeHorizon, confidence_level: confidence };
    }
    this.predictionStats.collision_runs++;
    this.predictionStats.last_collisions_ms = performance.now() - t0;
    if (this.opts.collisionCacheMs > 0) this.collisionCache.set(key, { at: wallMs, body });
    return { ...body, timestamp: wallMs };
  }

  onHttp(req, res) {
    const url = new URL(req.url || '/', 'http://gateway');
    const path = url.pathname;
    const json = (code, body) => {
      const s = JSON.stringify(body);
      res.writeHead(code, { 'content-type': 'application/json', 'cache-control': 'no-store' });
      res.end(s);
    };
    if (req.method !== 'GET' && req.method !== 'HEAD') {
      res.writeHead(405, { allow: 'GET, HEAD' });
      res.end();
      return;
    }
    try {
      if (path === '/gateway/stats') return json(200, this.stats());
      if (path === '/gateway/digest') return json(200, this.digest());
      if (path === '/api/aircraft') {
        return json(200, { now: Date.now(), aircraft: this.aircraftList(), timeWindow: 0, source: 'gateway' });
      }
      if (path === '/api/aircraft/enriched') return this.enriched(url.searchParams).then((b) => json(200, b), (e) => json(500, { error: e.message }));
      const trail = /^\/api\/aircraft\/([0-9a-fA-F]{1,6})\/trail$/.exec(path);
      if (trail) {
        const hex = trail[1].toUpperCase().padStart(6, '0');
        const limit = Math.max(1, Math.min(10_000, Number(url.searchParams.get('limit')) || 100));
        const points = (this.trails.get(Number.parseInt(hex, 16)) ?? []).slice(-limit);
        return json(200, { hex, trail: points, count: points.length });
      }
      if (path === '/api/statistics') return json(200, this.statistics());
      if (path === '/api/health') {
        return json(200, { status: 'ok', database: 'not used (gateway)', aircraft_count: this.book.size, timestamp: Date.now() });
      }
      if (path === '/api/collisions/kalman') return this.collisions(url.searchParams).then((b) => json(200, b), (e) => json(500, { error: e.message }));
      if (path.startsWith('/api/')) return json(404, { error: 'not provided by the gateway' });
      if (path.startsWith('/gateway/')) return this.serveFile(res, publicDir, path.slice('/gateway/'.length), req.method);
      return this.serveFile(res, this.opts.mapDir, path === '/' ? 'index.html' : path.slice(1), req.method);
    } catch (e) {
      return json(500, { error: e.message });
    }
  }

  // Static files, read-only, inside `root` only: no dotfiles (ads/.env holds
  // the database password), no node_modules, known types only.
  serveFile(res, root, rel, method) {
    let decoded;
    try {
      decoded = decodeURIComponent(rel);
    } catch {
      decoded = null;
    }
    const type = decoded === null ? undefined : contentTypes[extname(decoded).toLowerCase()];
    const full = decoded === null ? '' : normalize(join(root, decoded));
    const segments = decoded === null ? [] : decoded.split(/[\\/]/);
    const allowed =
      decoded !== null &&
      type !== undefined &&
      !decoded.includes('\0') &&
      (full === root || full.startsWith(root.endsWith(sep) ? root : root + sep)) &&
      !segments.some((s) => s.startsWith('.') || s === 'node_modules');
    let body = null;
    if (allowed) {
      try {
        if (statSync(full).isFile()) body = readFileSync(full);
      } catch {
        body = null;
      }
    }
    if (body === null) {
      res.writeHead(404, { 'content-type': 'text/plain' });
      res.end('not found');
      return;
    }
    res.writeHead(200, { 'content-type': type, 'cache-control': 'no-cache' });
    res.end(method === 'HEAD' ? undefined : body);
  }
}

