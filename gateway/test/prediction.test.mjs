// Probability cones and Kalman collisions (src/prediction.mjs) against the
// Fall 2025 originals (ads/kalman-filter.js, ads/probability-cone.js), and
// served by the gateway from its own trails in the feed's event time.
//
// The originals need mathjs, which the repository does not install. They take
// `window.math` when it exists, so this file gives them a small dense-matrix
// library with mathjs's semantics for the calls they make (products summed
// left to right, Gauss-Jordan inverse with partial pivoting). The port is
// specialised per axis; with the same inputs it must produce the same numbers.

import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, rmSync } from 'node:fs';
import http from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { Gateway } from '../src/gateway.mjs';
import {
  ProbabilityCone,
  detectCollisions,
  ecefToLatLonAlt,
  formatCollision,
  latLonAltToECEF,
} from '../src/prediction.mjs';
import { Rng, S1, trackUpdate } from './helpers.mjs';
import { NetPublisher, waitFor } from './net_fixture.mjs';

// ---- A mathjs stand-in for the original modules ---------------------------

class Mat {
  constructor(rows) {
    this.rows = rows.map((r) => r.slice());
  }
  get([i, j]) {
    return this.rows[i][j];
  }
  toArray() {
    return this.rows.map((r) => r.slice());
  }
}
const asRows = (m) => (m instanceof Mat ? m.rows : m);
const mathShim = {
  matrix: (rows) => new Mat(rows),
  zeros: (r, c) => new Mat(Array.from({ length: r }, () => new Array(c).fill(0))),
  identity: (n) => new Mat(Array.from({ length: n }, (_, i) => Array.from({ length: n }, (_, j) => (i === j ? 1 : 0)))),
  transpose: (m) => {
    const a = asRows(m);
    return new Mat(a[0].map((_, j) => a.map((r) => r[j])));
  },
  multiply: (x, y) => {
    if (typeof y === 'number') return new Mat(asRows(x).map((r) => r.map((v) => v * y)));
    if (typeof x === 'number') return new Mat(asRows(y).map((r) => r.map((v) => x * v)));
    const a = asRows(x);
    const b = asRows(y);
    return new Mat(
      a.map((ra) =>
        b[0].map((_, j) => {
          let sum = ra[0] * b[0][j];
          for (let k = 1; k < ra.length; k++) sum = sum + ra[k] * b[k][j];
          return sum;
        }),
      ),
    );
  },
  add: (x, y) => new Mat(asRows(x).map((r, i) => r.map((v, j) => v + asRows(y)[i][j]))),
  subtract: (x, y) => new Mat(asRows(x).map((r, i) => r.map((v, j) => v - asRows(y)[i][j]))),
  inv: (m) => {
    const A = asRows(m).map((r) => r.slice());
    const n = A.length;
    const B = asRows(mathShim.identity(n));
    for (let c = 0; c < n; c++) {
      let big = Math.abs(A[c][c]);
      let rBig = c;
      for (let r = c + 1; r < n; r++) {
        if (Math.abs(A[r][c]) > big) {
          big = Math.abs(A[r][c]);
          rBig = r;
        }
      }
      if (big === 0) throw new Error('singular');
      if (rBig !== c) {
        [A[c], A[rBig]] = [A[rBig], A[c]];
        [B[c], B[rBig]] = [B[rBig], B[c]];
      }
      const Ac = A[c];
      const Bc = B[c];
      for (let r = 0; r < n; r++) {
        const Ar = A[r];
        const Br = B[r];
        if (r !== c) {
          if (Ar[c] !== 0) {
            const f = -Ar[c] / Ac[c];
            for (let s = c; s < n; s++) Ar[s] = Ar[s] + f * Ac[s];
            for (let s = 0; s < n; s++) Br[s] = Br[s] + f * Bc[s];
          }
        } else {
          const f = Ac[c];
          for (let s = c; s < n; s++) Ar[s] = Ar[s] / f;
          for (let s = 0; s < n; s++) Br[s] = Br[s] / f;
        }
      }
    }
    return new Mat(B);
  },
  flatten: (m) => ({ toArray: () => asRows(m).flat() }),
};
globalThis.window = { math: mathShim };
const original = await import('../../ads/probability-cone.js');

// The originals read Date.now(); the port takes "now" as an argument.
function atTime(nowMs, fn) {
  const saved = Date.now;
  Date.now = () => nowMs;
  try {
    return fn();
  } finally {
    Date.now = saved;
  }
}

// ---- Scenarios ------------------------------------------------------------

// An aircraft flying a straight line at `kt` knots on `trackDeg`, one fix per
// `stepS` seconds, `n` fixes ending at `t0Ms + (n-1)*stepS*1000`.
function straightFlight({ hex, lat, lon, alt, kt, trackDeg, n = 30, stepS = 1, t0Ms = 1_000_000_000, vr = 0 }) {
  const ms = kt * 0.514444;
  const tr = (trackDeg * Math.PI) / 180;
  const history = [];
  let la = lat;
  let lo = lon;
  let al = alt;
  for (let i = 0; i < n; i++) {
    history.push({ lat: la, lon: lo, altitude: al, timestamp: t0Ms + i * stepS * 1000 });
    const mLat = 111320;
    const mLon = 111320 * Math.cos((la * Math.PI) / 180);
    la += (ms * stepS * Math.cos(tr)) / mLat;
    lo += (ms * stepS * Math.sin(tr)) / mLon;
    al += (vr / 60) * stepS;
  }
  const last = history[history.length - 1];
  const aircraft = { hex, lat: last.lat, lon: last.lon, altitude: last.altitude, speed: kt, track: trackDeg, vertical_rate: vr };
  return { aircraft, history, lastMs: last.timestamp };
}

function bearingDeg(lat1, lon1, lat2, lon2) {
  const r = Math.PI / 180;
  const y = Math.sin((lon2 - lon1) * r) * Math.cos(lat2 * r);
  const x = Math.cos(lat1 * r) * Math.sin(lat2 * r) - Math.sin(lat1 * r) * Math.cos(lat2 * r) * Math.cos((lon2 - lon1) * r);
  return ((Math.atan2(y, x) / r) + 360) % 360;
}

function groundDistanceM(a, b) {
  const p = latLonAltToECEF(a.lat, a.lon, 0);
  const q = latLonAltToECEF(b.lat, b.lon, 0);
  return Math.hypot(p.x - q.x, p.y - q.y, p.z - q.z);
}

// ---- The port reproduces the original -------------------------------------

test('cones equal the original ProbabilityCone on every path', () => {
  const f = straightFlight({ hex: 'ABC123', lat: 41.9, lon: -87.6, alt: 32000, kt: 450, trackDeg: 37, vr: 600 });
  const shuffled = [...f.history].reverse();
  shuffled.splice(3, 0, { ...f.history[5] }); // a duplicate time: skipped (dt = 0)
  const cases = [
    ['history, predicted forward to now', f.aircraft, f.history, f.lastMs + 2400],
    ['unsorted history with a duplicate', f.aircraft, shuffled, f.lastMs + 900],
    ['now equal to the last fix', f.aircraft, f.history, f.lastMs],
    ['history 5 minutes old: speed/track fallback', f.aircraft, f.history, f.lastMs + 300_000],
    ['one fix: speed/track', f.aircraft, f.history.slice(0, 1), f.lastMs],
    ['no history, no altitude', { ...f.aircraft, altitude: null, vertical_rate: null }, [], f.lastMs],
  ];
  for (const [what, ac, hist, now] of cases) {
    for (const [horizon, conf] of [[300, 0.95], [120, 0.99], [600, 0.5]]) {
      const want = atTime(now, () => new original.ProbabilityCone(ac, hist, horizon, conf).getPredictionPoints());
      const got = new ProbabilityCone(ac, hist, horizon, conf, now).getPredictionPoints();
      assert.deepEqual(got, want, `${what}, horizon ${horizon}, confidence ${conf}`);
      assert.equal(got.length, 20);
      for (const p of got) assert.deepEqual(Object.keys(p), ['time', 'lat', 'lon', 'altitude', 'radius']);
    }
  }
});

test('collisions equal the original detectCollisions + formatCollision (all pairs, no pruning)', () => {
  const rng = new Rng(7);
  const aircraft = [];
  for (let i = 0; i < 120; i++) {
    const f = straightFlight({
      hex: (0x400000 + i).toString(16).toUpperCase(),
      lat: 41.5 + rng.next() * 0.6,
      lon: -88 + rng.next() * 0.8,
      alt: [5000, 6000, 30000, 31000][rng.below(4)] + rng.below(400),
      kt: 150 + rng.below(350),
      trackDeg: rng.below(360),
      n: 5 + rng.below(20),
      t0Ms: 2_000_000_000 + rng.below(5000),
      vr: rng.below(3) === 0 ? 0 : (rng.below(2) ? 1 : -1) * rng.below(1500),
    });
    aircraft.push({ ...f.aircraft, positionHistory: f.history });
  }
  const now = 2_000_030_000;
  for (const [horizon, conf] of [[300, 0.95], [180, 0.99]]) {
    const want = atTime(now, () => original.detectCollisions(aircraft, horizon, conf).map(original.formatCollision));
    const got = detectCollisions(aircraft, horizon, conf, now).map(formatCollision);
    assert.ok(want.length >= 5, `scenario too sparse: ${want.length} collisions`);
    assert.deepEqual(got, want);
    // The raw objects too, not only the rounded ones.
    const wantRaw = atTime(now, () => original.detectCollisions(aircraft, horizon, conf));
    assert.deepEqual(detectCollisions(aircraft, horizon, conf, now), wantRaw);
  }
});

// ---- Behaviour ------------------------------------------------------------

test('a straight-flying aircraft: the cone points along its track and widens with time', () => {
  for (const trackDeg of [0, 45, 90, 200, 315]) {
    const f = straightFlight({ hex: 'A00001', lat: 40, lon: -88, alt: 20000, kt: 400, trackDeg, n: 40 });
    const pts = new ProbabilityCone(f.aircraft, f.history, 300, 0.95, f.lastMs).getPredictionPoints();
    let prevD = 0;
    let prevR = 0;
    for (const p of pts) {
      const d = groundDistanceM(f.aircraft, p);
      const expected = 400 * 0.514444 * p.time;
      assert.ok(Math.abs(d - expected) < 0.02 * expected, `track ${trackDeg}, t ${p.time}: ${d} m vs ${expected} m`);
      const b = bearingDeg(f.aircraft.lat, f.aircraft.lon, p.lat, p.lon);
      const off = Math.abs(((b - trackDeg + 540) % 360) - 180);
      assert.ok(off < 1, `track ${trackDeg}, t ${p.time}: bearing ${b}`);
      // Level flight; a constant ECEF velocity rises above the curved Earth by d^2 / 2R.
      const rise = (d * d) / (2 * 6371000) / 0.3048;
      assert.ok(Math.abs(p.altitude - 20000 - rise) < 50, `level flight: ${p.altitude} ft (curvature ${rise} ft)`);
      assert.ok(d > prevD && p.radius > prevR, 'moves on and widens');
      prevD = d;
      prevR = p.radius;
    }
  }
});

test('converging aircraft are reported, diverging and vertically separated ones are not', () => {
  const t0Ms = 5_000_000;
  const now = t0Ms + 19_000;
  const mk = (hex, lat, lon, alt, trackDeg) => {
    const f = straightFlight({ hex, lat, lon, alt, kt: 420, trackDeg, n: 20, t0Ms });
    return { ...f.aircraft, positionHistory: f.history };
  };
  const list = [
    mk('C00001', 40, -88.3, 30000, 90), // head-on pair, about 25 km apart
    mk('C00002', 40, -88.0, 30000, 270),
    mk('D00001', 42, -88.3, 30000, 270), // flying apart
    mk('D00002', 42, -88.0, 30000, 90),
    mk('E00001', 44, -88.3, 30000, 90), // head-on but 4,000 ft apart
    mk('E00002', 44, -88.0, 34000, 270),
  ];
  const found = detectCollisions(list, 300, 0.95, now);
  assert.deepEqual(found.map((c) => [c.aircraft1, c.aircraft2]), [['C00001', 'C00002']]);
  const c = formatCollision(found[0]);
  // They meet after about 25.6 km / (2 * 216 m/s) - the 1-20 s already flown.
  assert.ok(c.timeToCollision > 15 && c.timeToCollision < 75, `time ${c.timeToCollision}`);
  assert.match(c.probability, /^\d+\.\d%$/);
});

// ---- Served by the gateway ------------------------------------------------

function get(port, path) {
  return new Promise((resolve, reject) => {
    http
      .get({ host: '127.0.0.1', port, path }, (res) => {
        const chunks = [];
        res.on('data', (d) => chunks.push(d));
        res.on('end', () => resolve(JSON.parse(Buffer.concat(chunks).toString())));
      })
      .on('error', reject);
  });
}

async function gatewayWith(gwOpts = {}) {
  const pub = await new NetPublisher(S1).open();
  const base = mkdtempSync(join(tmpdir(), 'gw-cones-'));
  mkdirSync(join(base, 'map'));
  const gw = new Gateway({
    listen: { host: '127.0.0.1', port: 0 },
    mapDir: join(base, 'map'),
    clockHelper: null,
    statusMs: 50,
    ...gwOpts,
    feedOptions: { feed: { host: '127.0.0.1', port: 0 }, retransmit: pub.retransmit, snapshot: pub.snapshot, pollMs: 1 },
  });
  await gw.start();
  pub.dest = { host: '127.0.0.1', port: gw.feed.feedPort() };
  const cleanup = async () => {
    await gw.stop().catch(() => {});
    pub.close();
    rmSync(base, { recursive: true, force: true });
  };
  return { pub, gw, port: gw.address().port, cleanup };
}

// Feed updates for a flight: one position update per fix, event time = the
// fix's timestamp (ms -> ns), speed/track/vertical rate on the wire.
function flightUpdates(f, icao) {
  return f.history.map((p) =>
    trackUpdate({
      icao,
      lat: p.lat,
      lon: p.lon,
      alt: Math.round(p.altitude),
      mask: 1 | 2 | 4 | 8,
      eventTs: BigInt(p.timestamp) * 1_000_000n,
      callsign: `F${icao.toString(16)}`,
      gsDkt: f.aircraft.speed * 10,
      trackCdeg: f.aircraft.track * 100,
      vrateFpm: 0,
    }),
  );
}

test('gateway: enriched cones and Kalman collisions from its trails, in event time', async () => {
  const { pub, gw, port, cleanup } = await gatewayWith();
  try {
    const t0Ms = 1_700_000_000; // event time far from the wall clock: Date.now() would be the wrong "now"
    const flights = [
      [0xc00001, straightFlight({ hex: 'C00001', lat: 40, lon: -88.3, alt: 30000, kt: 420, trackDeg: 90, n: 20, t0Ms })],
      [0xc00002, straightFlight({ hex: 'C00002', lat: 40, lon: -88.0, alt: 30000, kt: 420, trackDeg: 270, n: 20, t0Ms })],
      [0xd00001, straightFlight({ hex: 'D00001', lat: 42, lon: -88.3, alt: 30000, kt: 420, trackDeg: 270, n: 20, t0Ms })],
      [0xd00002, straightFlight({ hex: 'D00002', lat: 42, lon: -88.0, alt: 30000, kt: 420, trackDeg: 90, n: 20, t0Ms })],
    ];
    const msgs = [];
    for (let i = 0; i < 20; i++) for (const [icao, f] of flights) msgs.push(flightUpdates(f, icao)[i]);
    pub.publish(msgs);
    await waitFor(() => gw.core.expected === msgs.length + 1, 3000, 'applied');
    assert.equal(gw.predictionStats.cones_computed, 0, 'nothing computed on the feed path');

    const enr = await get(port, '/api/aircraft/enriched?window=0&trails=true&cones=true');
    assert.equal(enr.stats.withCones, 4);
    const nowMs = t0Ms + 19_000; // newest event time in the book
    for (const [icao, f] of flights) {
      const a = enr.aircraft.find((x) => x.hex === f.aircraft.hex);
      assert.equal(a.trail.length, 20);
      assert.equal(a.trail[19].event_ms, f.lastMs);
      // The same cone as the original would draw for this trail and "now".
      const hist = a.trail.map((p) => ({ lat: p.lat, lon: p.lon, altitude: p.altitude, timestamp: p.event_ms }));
      const want = atTime(nowMs, () => new original.ProbabilityCone(a, hist, 300, 0.95).getPredictionPoints());
      assert.deepEqual(a.probabilityCone, want, `cone of ${icao.toString(16)}`);
    }

    assert.equal(gw.predictionStats.cones_computed, 4);
    const col = await get(port, '/api/collisions/kalman');
    assert.equal(gw.predictionStats.cones_computed, 4, 'same trail points: the enriched cones are reused');
    assert.equal(gw.predictionStats.cone_cache_hits, 4);
    assert.equal(col.aircraft_count, 4);
    assert.deepEqual(col.collisions.map((c) => [c.aircraft1, c.aircraft2]), [['C00001', 'C00002']]);
    const c = col.collisions[0];
    assert.equal(c.flight1, 'Fc00001');
    assert.deepEqual(Object.keys(c.currentPosition1), ['lat', 'lon', 'alt']);
    assert.equal(c.currentPosition2.alt, 30000);
    // As the Fall 2025 endpoint computes it for the same aircraft and history.
    const list = enr.aircraft.map((a) => ({
      ...a,
      positionHistory: a.trail.map((p) => ({ lat: p.lat, lon: p.lon, altitude: p.altitude, timestamp: p.event_ms })),
    }));
    const want = atTime(nowMs, () => original.detectCollisions(list, 300, 0.95).map(original.formatCollision));
    const { currentPosition1, currentPosition2, flight1, flight2, ...core } = c;
    assert.deepEqual(core, want[0]);
    assert.equal(want.length, 1);

    // Cached: a second request within collisionCacheMs computes nothing.
    const runs = gw.predictionStats.collision_runs;
    assert.equal((await get(port, '/api/collisions/kalman')).cached, true);
    assert.equal(gw.predictionStats.collision_runs, runs);
    const computed = gw.predictionStats.cones_computed;
    const again = await get(port, '/api/aircraft/enriched');
    assert.deepEqual(again.aircraft.map((a) => a.probabilityCone), enr.aircraft.map((a) => a.probabilityCone));
    assert.equal(gw.predictionStats.cones_computed, computed, 'cones reused within coneCacheMs');
    assert.equal(gw.predictionStats.cone_cache_hits, 8);

    // Another aircraft 200 s later in event time: the four are no longer
    // active (last update over 2 minutes before "now"), although their
    // positions are still within the 10-minute history window.
    pub.publish([trackUpdate({ icao: 0xe00001, lat: 45, lon: -80, alt: 10000, mask: 1, eventTs: BigInt(nowMs + 200_000) * 1_000_000n })]);
    await waitFor(() => gw.core.expected === msgs.length + 2, 3000, 'applied');
    const later = await get(port, '/api/collisions/kalman?time_horizon=301');
    assert.equal(later.aircraft_count, 0);
    assert.deepEqual(later.collisions, []);
  } finally {
    await cleanup();
  }
});

test('gateway: cones=false omits cones; no history and no speed/track -> null cone; too few aircraft -> message', async () => {
  const { pub, gw, port, cleanup } = await gatewayWith({ coneCacheMs: 0, collisionCacheMs: 0 });
  try {
    pub.publish([
      trackUpdate({ icao: 0xa00001, lat: 40, lon: -88, alt: 10000, mask: 1, eventTs: 5_000_000_000 }), // one fix, no speed
      trackUpdate({ icao: 0xa00002, lat: 41, lon: -88, alt: 10000, mask: 1 | 4 | 8, eventTs: 5_000_000_000, gsDkt: 2500, trackCdeg: 9000 }),
    ]);
    await waitFor(() => gw.core.expected === 3, 3000, 'applied');
    const off = await get(port, '/api/aircraft/enriched?cones=false');
    assert.equal(off.aircraft.length, 2);
    for (const a of off.aircraft) assert.equal('probabilityCone' in a, false);
    assert.equal(off.stats.withCones, 0);
    assert.equal(gw.predictionStats.cones_computed, 0);
    const notrails = await get(port, '/api/aircraft/enriched?trails=false');
    for (const a of notrails.aircraft) assert.equal('trail' in a, false);
    const on = await get(port, '/api/aircraft/enriched');
    assert.equal(on.aircraft.find((a) => a.hex === 'A00001').probabilityCone, null);
    const cone = on.aircraft.find((a) => a.hex === 'A00002').probabilityCone;
    assert.equal(cone.length, 20);
    assert.ok(cone[19].lon > -88 + 0.3, 'flies east from speed and track alone');
    const col = await get(port, '/api/collisions/kalman');
    assert.deepEqual(col.collisions, []);
    assert.equal(col.aircraft_count, 0);
    assert.match(col.message, /Not enough aircraft/);
  } finally {
    await cleanup();
  }
});
