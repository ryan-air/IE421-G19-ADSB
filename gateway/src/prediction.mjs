// Probability cones and the Kalman collision check of the Fall 2025 map
// (ads/kalman-filter.js, ads/probability-cone.js), ported for the gateway.
//
// The originals need mathjs (an npm package) and read Date.now(); the gateway
// has no dependencies and works in the feed's event time. The algorithm is the
// same, step for step: the same constant-velocity model, noise matrices,
// initialisation from the first two positions, training on the history,
// forward prediction to "now" (or the track/speed fallback when the history is
// 5 minutes old or more), 20 prediction steps, chi-squared radii, and the same
// intersection test and output objects.
//
// Only the matrix arithmetic is specialised. F, Q, R and H act on each ECEF
// axis separately and the initial covariance is diagonal, so the 6x6
// covariance only ever has entries (i,i), (i,i+3), (i+3,i), (i+3,i+3): three
// independent 2-state filters. Each entry is computed with the non-zero terms
// of the generic matrix products, in the same order, so the results equal the
// generic 6x6 computation (gateway/test/prediction.test.mjs runs the original
// modules with a plain matrix library and compares). A cone costs about 220
// filter steps of a few multiplications instead of 6x6 matrix products.

// ---- Coordinates (verbatim from ads/kalman-filter.js) ----------------------

const WGS84_A = 6378137.0; // semi-major axis (m)
const WGS84_E2 = 0.00669437999014; // first eccentricity squared

/** lat/lon in degrees, altitude in feet -> ECEF {x, y, z} in metres. */
export function latLonAltToECEF(lat, lon, alt) {
  const latRad = (lat * Math.PI) / 180;
  const lonRad = (lon * Math.PI) / 180;
  const altMeters = alt * 0.3048;
  const N = WGS84_A / Math.sqrt(1 - WGS84_E2 * Math.sin(latRad) * Math.sin(latRad));
  const x = (N + altMeters) * Math.cos(latRad) * Math.cos(lonRad);
  const y = (N + altMeters) * Math.cos(latRad) * Math.sin(lonRad);
  const z = (N * (1 - WGS84_E2) + altMeters) * Math.sin(latRad);
  return { x, y, z };
}

/** ECEF metres -> {lat, lon} in degrees, alt in feet. */
export function ecefToLatLonAlt(x, y, z) {
  const lon = Math.atan2(y, x);
  const p = Math.sqrt(x * x + y * y);
  let lat = Math.atan2(z, p * (1 - WGS84_E2));
  for (let i = 0; i < 5; i++) {
    const N = WGS84_A / Math.sqrt(1 - WGS84_E2 * Math.sin(lat) * Math.sin(lat));
    const h = p / Math.cos(lat) - N;
    lat = Math.atan2(z, p * (1 - WGS84_E2 * N / (N + h)));
  }
  const N = WGS84_A / Math.sqrt(1 - WGS84_E2 * Math.sin(lat) * Math.sin(lat));
  const alt = p / Math.cos(lat) - N;
  return { lat: (lat * 180) / Math.PI, lon: (lon * 180) / Math.PI, alt: alt / 0.3048 };
}

export function distance3D(p1, p2) {
  const dx = p2.x - p1.x;
  const dy = p2.y - p1.y;
  const dz = p2.z - p1.z;
  return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- Kalman filter ---------------------------------------------------------

const kQPos = 1; // process noise, position (diagonal of Q)
const kQVel = 10; // process noise, velocity
const kR = [50, 50, 100]; // measurement noise per axis (diagonal of R)
const kP0 = 1000; // initial covariance, identity * 1000

/** State [x, y, z, vx, vy, vz]; covariance as three per-axis 2x2 blocks. */
export class KalmanFilter {
  constructor(initialState) {
    this.state = Float64Array.from(initialState ?? [0, 0, 0, 0, 0, 0]);
    // Per axis: pp = P[i][i], pv = P[i][i+3], vp = P[i+3][i], vv = P[i+3][i+3].
    this.pp = [kP0, kP0, kP0];
    this.pv = [0, 0, 0];
    this.vp = [0, 0, 0];
    this.vv = [kP0, kP0, kP0];
  }

  predict(dt) {
    const s = this.state;
    for (let i = 0; i < 3; i++) {
      s[i] = s[i] + dt * s[i + 3];
      // F P: row i = P[i] + dt * P[i+3]; row i+3 unchanged.
      const fpp = this.pp[i] + dt * this.vp[i];
      const fpv = this.pv[i] + dt * this.vv[i];
      // (F P) F^T + Q: column i = (FP)[., i] + (FP)[., i+3] * dt.
      this.pp[i] = fpp + fpv * dt + kQPos;
      // this.pv[i] = fpv + 0 (Q has no off-diagonal terms)
      this.pv[i] = fpv;
      this.vp[i] = this.vp[i] + this.vv[i] * dt;
      this.vv[i] = this.vv[i] + kQVel;
    }
    return this.getStateArray();
  }

  update(measurement) {
    const s = this.state;
    for (let i = 0; i < 3; i++) {
      const innovation = measurement[i] - s[i];
      const sInv = 1 / (this.pp[i] + kR[i]); // S is diagonal
      const kp = this.pp[i] * sInv;
      const kv = this.vp[i] * sInv;
      s[i] = s[i] + kp * innovation;
      s[i + 3] = s[i + 3] + kv * innovation;
      // P = (I - K H) P
      const a = this.pp[i];
      const b = this.pv[i];
      const ikh = 1 - kp;
      this.pp[i] = ikh * a;
      this.pv[i] = ikh * b;
      this.vp[i] = -kv * a + this.vp[i];
      this.vv[i] = -kv * b + this.vv[i];
    }
  }

  getStateArray() {
    const s = this.state;
    return [s[0], s[1], s[2], s[3], s[4], s[5]]; // (Array.from of a typed array is 10x slower)
  }

  getPositionUncertainty() {
    return { x: Math.sqrt(this.pp[0]), y: Math.sqrt(this.pp[1]), z: Math.sqrt(this.pp[2]) };
  }
}

// ---- Probability cone ------------------------------------------------------

// Velocity from speed/track/vertical rate as the position change over 1 s
// (both of the original's fallbacks compute exactly this).
function stateFromTrack(aircraft) {
  const currentECEF = latLonAltToECEF(aircraft.lat, aircraft.lon, aircraft.altitude);
  const speedMS = (aircraft.speed || 0) * 0.514444;
  const trackRad = ((aircraft.track || 0) * Math.PI) / 180;
  const verticalRateFtPerSec = (aircraft.vertical_rate || 0) / 60;
  const latRad = (aircraft.lat * Math.PI) / 180;
  const metersPerDegreeLat = 111320;
  const metersPerDegreeLon = 111320 * Math.cos(latRad);
  const deltaLatPerSec = (speedMS * Math.cos(trackRad)) / metersPerDegreeLat;
  const deltaLonPerSec = (speedMS * Math.sin(trackRad)) / metersPerDegreeLon;
  const futureECEF = latLonAltToECEF(
    aircraft.lat + deltaLatPerSec,
    aircraft.lon + deltaLonPerSec,
    aircraft.altitude + verticalRateFtPerSec,
  );
  return [
    currentECEF.x,
    currentECEF.y,
    currentECEF.z,
    futureECEF.x - currentECEF.x,
    futureECEF.y - currentECEF.y,
    futureECEF.z - currentECEF.z,
  ];
}

// A timestamp as the original reads it (new Date(t)), in ms. Plain numbers,
// the gateway's case, skip the Date object: same value (Date truncates to a
// whole ms and is NaN beyond +-8.64e15).
function ms(t) {
  if (typeof t === 'number') return Math.abs(t) <= 8.64e15 ? Math.trunc(t) + 0 : NaN;
  return new Date(t).getTime();
}

export class ProbabilityCone {
  /**
   * @param aircraft {hex, lat, lon, altitude, speed, track, vertical_rate}
   * @param positionHistory [{lat, lon, altitude, timestamp}], timestamp in ms
   *        of event time (anything `new Date()` accepts, as in the original)
   * @param predictionTimeHorizon seconds
   * @param confidenceLevel 0.95 or 0.99 (chi-squared 7.815 / 11.345)
   * @param nowMs event time "now" (the original used Date.now())
   */
  constructor(aircraft, positionHistory, predictionTimeHorizon = 300, confidenceLevel = 0.95, nowMs = Date.now()) {
    this.aircraft = aircraft;
    this.positionHistory = positionHistory;
    this.predictionTimeHorizon = predictionTimeHorizon;
    this.confidenceLevel = confidenceLevel;
    this.nowMs = nowMs;
    this.chiSquared = confidenceLevel >= 0.99 ? 11.345 : 7.815;
    this.kalmanFilter = this.initializeKalmanFilter();
    this.predictions = this.generatePredictions();
  }

  initializeKalmanFilter() {
    if (!this.positionHistory || this.positionHistory.length < 2) {
      return new KalmanFilter(stateFromTrack(this.aircraft));
    }
    const sorted = [...this.positionHistory];
    let inOrder = true;
    for (let i = 1; i < sorted.length && inOrder; i++) inOrder = ms(sorted[i - 1].timestamp) <= ms(sorted[i].timestamp);
    if (!inOrder) sorted.sort((a, b) => ms(a.timestamp) - ms(b.timestamp)); // stable, as the original's
    const first = sorted[0];
    const firstECEF = latLonAltToECEF(first.lat, first.lon, first.altitude);
    let vx = 0;
    let vy = 0;
    let vz = 0;
    const second = sorted[1];
    const secondECEF = latLonAltToECEF(second.lat, second.lon, second.altitude);
    const dt0 = (ms(second.timestamp) - ms(first.timestamp)) / 1000;
    if (dt0 > 0) {
      vx = (secondECEF.x - firstECEF.x) / dt0;
      vy = (secondECEF.y - firstECEF.y) / dt0;
      vz = (secondECEF.z - firstECEF.z) / dt0;
    }
    const kf = new KalmanFilter([firstECEF.x, firstECEF.y, firstECEF.z, vx, vy, vz]);
    let prevTime = ms(first.timestamp);
    let lastHistoricalTime = prevTime;
    for (let i = 1; i < sorted.length; i++) {
      const pos = sorted[i];
      const currentTime = ms(pos.timestamp);
      const dt = (currentTime - prevTime) / 1000;
      if (dt > 0) {
        kf.predict(dt);
        const ecef = latLonAltToECEF(pos.lat, pos.lon, pos.altitude);
        kf.update([ecef.x, ecef.y, ecef.z]);
        prevTime = currentTime;
        lastHistoricalTime = currentTime;
      }
    }
    // Predict forward from the last position to now (up to 5 minutes); older
    // history: start from the current position, speed and track instead.
    const timeSinceLastUpdate = (this.nowMs - lastHistoricalTime) / 1000;
    if (timeSinceLastUpdate > 0 && timeSinceLastUpdate < 300) {
      kf.predict(timeSinceLastUpdate);
    } else if (timeSinceLastUpdate >= 300) {
      return new KalmanFilter(stateFromTrack(this.aircraft));
    }
    return kf;
  }

  generatePredictions() {
    const predictions = [];
    const timeSteps = 20;
    const dt = this.predictionTimeHorizon / timeSteps;
    for (let i = 1; i <= timeSteps; i++) {
      const time = i * dt;
      const state = this.kalmanFilter.predict(dt);
      const position = { x: state[0], y: state[1], z: state[2] };
      const velocity = { vx: state[3], vy: state[4], vz: state[5] };
      const uncertainty = this.kalmanFilter.getPositionUncertainty();
      predictions.push({
        time,
        position,
        latLonAlt: ecefToLatLonAlt(position.x, position.y, position.z),
        velocity,
        uncertainty,
        radius: Math.sqrt(uncertainty.x * uncertainty.x + uncertainty.y * uncertainty.y + uncertainty.z * uncertainty.z),
      });
    }
    return predictions;
  }

  /** The objects /api/aircraft/enriched returns as `probabilityCone`. */
  getPredictionPoints() {
    const k = Math.sqrt(this.chiSquared);
    return this.predictions.map((p) => ({
      time: p.time,
      lat: p.latLonAlt.lat,
      lon: p.latLonAlt.lon,
      altitude: p.latLonAlt.alt,
      radius: p.radius * k,
    }));
  }
}

// ---- Collisions ------------------------------------------------------------

/** The original's checkConeIntersection: first step at which the cones meet. */
export function checkConeIntersection(cone1, cone2, minVerticalSeparation = 1000) {
  const predictions1 = cone1.predictions;
  const predictions2 = cone2.predictions;
  for (let i = 0; i < Math.min(predictions1.length, predictions2.length); i++) {
    const p1 = predictions1[i];
    const p2 = predictions2[i];
    const altitudeDiff = Math.abs(p1.latLonAlt.alt - p2.latLonAlt.alt);
    const maxAlt = Math.max(p1.latLonAlt.alt, p2.latLonAlt.alt);
    const requiredVerticalSep = maxAlt > 29000 ? 2000 : minVerticalSeparation;
    if (altitudeDiff > requiredVerticalSep) continue;
    const dist = distance3D(p1.position, p2.position);
    const radius1 = p1.radius * Math.sqrt(cone1.chiSquared);
    const radius2 = p2.radius * Math.sqrt(cone2.chiSquared);
    const combinedRadius = radius1 + radius2;
    if (dist <= combinedRadius) {
      const overlapRatio = Math.max(0, 1 - dist / combinedRadius);
      const baseProbability = cone1.confidenceLevel * cone2.confidenceLevel;
      return {
        time: p1.time,
        probability: baseProbability * overlapRatio,
        distance: dist,
        position1: p1.latLonAlt,
        position2: p2.latLonAlt,
        verticalSeparation: altitudeDiff,
        uncertainty1: radius1,
        uncertainty2: radius2,
        aircraft1: cone1.aircraft.hex,
        aircraft2: cone2.aircraft.hex,
      };
    }
  }
  return null;
}

// ECEF bounding box of a cone's predicted positions, widened by its largest
// radius (plus 1 m for rounding). Two cones whose boxes do not overlap cannot
// meet at any step (distance >= |difference on one axis| > r1 + r2), so the
// pair is skipped without changing the result.
function coneBox(cone) {
  const k = Math.sqrt(cone.chiSquared);
  let r = 0;
  const lo = [Infinity, Infinity, Infinity];
  const hi = [-Infinity, -Infinity, -Infinity];
  for (const p of cone.predictions) {
    r = Math.max(r, p.radius * k);
    const q = [p.position.x, p.position.y, p.position.z];
    for (let a = 0; a < 3; a++) {
      lo[a] = Math.min(lo[a], q[a]);
      hi[a] = Math.max(hi[a], q[a]);
    }
  }
  r += 1;
  // NaN positions (bad input) never prune: the exact test decides.
  for (let a = 0; a < 3; a++) {
    lo[a] = Number.isNaN(lo[a] - r) ? -Infinity : lo[a] - r;
    hi[a] = Number.isNaN(hi[a] + r) ? Infinity : hi[a] + r;
  }
  return { lo, hi };
}

/**
 * The original's detectCollisions, given the cones (so callers can cache
 * them): every pair, in index order, then sorted by time (stable).
 * Pairs are pre-filtered by bounding boxes (sweep along x); the result is the
 * same as checking every pair.
 */
export function detectCollisionsFromCones(cones) {
  const boxes = cones.map(coneBox);
  const order = cones.map((_, i) => i).sort((a, b) => boxes[a].lo[0] - boxes[b].lo[0] || a - b);
  const found = [];
  for (let oi = 0; oi < order.length; oi++) {
    const i = order[oi];
    const bi = boxes[i];
    for (let oj = oi + 1; oj < order.length; oj++) {
      const j = order[oj];
      const bj = boxes[j];
      if (bj.lo[0] > bi.hi[0]) break;
      if (bj.lo[1] > bi.hi[1] || bi.lo[1] > bj.hi[1] || bj.lo[2] > bi.hi[2] || bi.lo[2] > bj.hi[2]) continue;
      const [a, b] = i < j ? [i, j] : [j, i];
      const collision = checkConeIntersection(cones[a], cones[b]);
      if (collision) found.push({ a, b, collision });
    }
  }
  found.sort((x, y) => x.collision.time - y.collision.time || x.a - y.a || x.b - y.b);
  return found.map((f) => f.collision);
}

/** The original's detectCollisions(aircraftList, horizon, confidence, nowMs). */
export function detectCollisions(aircraftList, predictionTimeHorizon = 300, confidenceLevel = 0.95, nowMs = Date.now()) {
  const cones = [];
  for (const aircraft of aircraftList) {
    try {
      cones.push(new ProbabilityCone(aircraft, aircraft.positionHistory || [], predictionTimeHorizon, confidenceLevel, nowMs));
    } catch {
      // as the original: an aircraft whose cone fails is left out
    }
  }
  return detectCollisionsFromCones(cones);
}

/** The original's formatCollision (strings and rounding as the map expects). */
export function formatCollision(collision) {
  return {
    aircraft1: collision.aircraft1,
    aircraft2: collision.aircraft2,
    timeToCollision: Math.round(collision.time),
    probability: (collision.probability * 100).toFixed(1) + '%',
    distance: Math.round(collision.distance),
    verticalSeparation: Math.round(collision.verticalSeparation),
    position1: {
      lat: collision.position1.lat.toFixed(4),
      lon: collision.position1.lon.toFixed(4),
      alt: Math.round(collision.position1.alt),
    },
    position2: {
      lat: collision.position2.lat.toFixed(4),
      lon: collision.position2.lon.toFixed(4),
      alt: Math.round(collision.position2.alt),
    },
  };
}
