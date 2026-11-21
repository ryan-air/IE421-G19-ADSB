// Clocks and latency histograms for the gateway.
//
// TrackUpdate.publish_ts_ns is the publisher's CLOCK_MONOTONIC_RAW. Node's
// process.hrtime() is CLOCK_MONOTONIC on Linux (NTP-slewed, so it drifts from
// RAW) and mach_absolute_time on macOS. To compare the two, ClockSync runs the
// `adsb-clock` helper (core/tools/adsb_clock.cpp), which answers each byte on
// stdin with one line holding mono_ns(). Of 32 queries the one with the
// shortest round trip gives the offset, to within half that round trip. It is
// redone every 10 s to follow slew.
//
// Without the helper the offset is 0 and latencies are labelled
// "uncalibrated": fine on macOS today (the clocks happen to agree within the
// spawn measurement error), wrong on Linux after long uptimes.

import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

export const hrNs = () => Number(process.hrtime.bigint());

export class ClockSync {
  constructor(helperPath = null, { samples = 32, intervalMs = 10_000 } = {}) {
    this.helperPath = helperPath;
    this.samples = samples;
    this.intervalMs = intervalMs;
    this.offsetNs = 0; // publisher mono_ns ≈ hrNs() + offsetNs
    this.bestRttNs = null;
    this.calibrated = false;
    this.child = null;
    this.timer = null;
  }

  // Publisher-clock time now.
  now() {
    return hrNs() + this.offsetNs;
  }

  async start() {
    if (!this.helperPath) return false;
    this.child = spawn(this.helperPath, [], { stdio: ['pipe', 'pipe', 'inherit'] });
    this.child.on('error', () => {});
    this.lines = createInterface({ input: this.child.stdout })[Symbol.asyncIterator]();
    const ok = await this.calibrate();
    this.timer = setInterval(() => this.calibrate().catch(() => {}), this.intervalMs);
    this.timer.unref();
    return ok;
  }

  async query() {
    const t0 = hrNs();
    this.child.stdin.write('q');
    const { value, done } = await this.lines.next();
    const t1 = hrNs();
    if (done) throw new Error('adsb-clock exited');
    return { rtt: t1 - t0, offset: Number(value) - (t0 + t1) / 2 };
  }

  async calibrate() {
    if (this.busy || !this.child) return this.calibrated;
    this.busy = true;
    try {
      let best = null;
      for (let i = 0; i < this.samples; i++) {
        const s = await this.query();
        if (!best || s.rtt < best.rtt) best = s;
      }
      this.offsetNs = Math.round(best.offset);
      this.bestRttNs = best.rtt;
      this.calibrated = true;
    } catch {
      // Keep the last offset.
    } finally {
      this.busy = false;
    }
    return this.calibrated;
  }

  stop() {
    if (this.timer) clearInterval(this.timer);
    if (this.child) {
      this.child.stdin.end();
      this.child.kill();
    }
  }

  describe() {
    return {
      calibrated: this.calibrated,
      offset_ns: this.offsetNs,
      uncertainty_ns: this.bestRttNs === null ? null : Math.ceil(this.bestRttNs / 2),
    };
  }
}

// Log-spaced bucket index for an integer number of µs: buckets 0-3 are 1 µs
// wide, then four buckets per octave ([4,5), [5,6), [6,7), [7,8), [8,10) ...),
// so each is at most 25% wide. 76 buckets cover 0 µs to 1 s.
export const kLogBuckets = 76;

export function logBucket(us) {
  if (us < 4) return us;
  const octave = 31 - Math.clz32(us);
  return 4 + (octave - 2) * 4 + ((us >>> (octave - 2)) & 3);
}

// [lower, upper) edges of a log bucket, in µs.
export function logBucketEdges(i) {
  if (i < 4) return [i, i + 1];
  const octave = 2 + ((i - 4) >> 2);
  const sub = (i - 4) & 3;
  return [(4 + sub) * 2 ** (octave - 2), (5 + sub) * 2 ** (octave - 2)];
}

// Latency histogram with 1 µs buckets up to 1 s (plus overflow): exact enough
// for percentiles at these scales, fixed memory, O(1) record. A second,
// log-spaced copy (76 buckets) is kept for clients that draw the distribution
// or diff two readings for a windowed percentile: reading the fine histogram
// means scanning a million buckets on the event loop.
export class LatencyHistogram {
  static kBuckets = 1_000_000;

  constructor() {
    this.counts = new Uint32Array(LatencyHistogram.kBuckets);
    this.logCounts = new Float64Array(kLogBuckets);
    this.reset();
  }

  reset() {
    this.counts.fill(0);
    this.logCounts.fill(0);
    this.n = 0;
    this.overflow = 0;
    this.negative = 0;
    this.sumNs = 0;
    this.maxNs = 0;
  }

  record(ns) {
    if (ns < 0) {
      this.negative++; // clock offset error: excluded, but counted
      return;
    }
    const us = Math.floor(ns / 1000);
    if (us >= LatencyHistogram.kBuckets) this.overflow++;
    else {
      this.counts[us]++;
      this.logCounts[logBucket(us)]++;
    }
    this.n++;
    this.sumNs += ns;
    if (ns > this.maxNs) this.maxNs = ns;
  }

  // Upper edge of the bucket holding quantile q, in µs.
  percentileUs(q) {
    if (this.n === 0) return null;
    const rank = Math.max(1, Math.ceil(q * this.n));
    let seen = 0;
    for (let i = 0; i < this.counts.length; i++) {
      seen += this.counts[i];
      if (seen >= rank) return i + 1;
    }
    return Infinity;
  }

  // Cumulative counts per log bucket since start (or reset); `edges_us` has
  // one more entry than `counts`. Samples of 1 s or more are in `over_1s`.
  logHistogram() {
    const edges = [];
    for (let i = 0; i < kLogBuckets; i++) edges.push(logBucketEdges(i)[0]);
    edges.push(logBucketEdges(kLogBuckets - 1)[1]);
    return { edges_us: edges, counts: Array.from(this.logCounts), over_1s: this.overflow, count: this.n };
  }

  summary() {
    return {
      count: this.n,
      p50_us: this.percentileUs(0.5),
      p90_us: this.percentileUs(0.9),
      p99_us: this.percentileUs(0.99),
      p999_us: this.percentileUs(0.999),
      max_us: this.n ? Math.ceil(this.maxNs / 1000) : null,
      mean_us: this.n ? Math.round(this.sumNs / this.n / 100) / 10 : null,
      over_1s: this.overflow,
      negative: this.negative,
    };
  }
}
