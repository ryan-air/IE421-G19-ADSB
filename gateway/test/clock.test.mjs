// LatencyHistogram: the log-spaced copy used by /gateway/latency must agree
// with the fine histogram it summarises.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LatencyHistogram, kLogBuckets, logBucket, logBucketEdges } from '../src/clock.mjs';
import { Rng } from './helpers.mjs';

test('log buckets tile 0 µs to 1 s without gaps, each at most 25% wide', () => {
  let upper = 0;
  for (let i = 0; i < kLogBuckets; i++) {
    const [lo, hi] = logBucketEdges(i);
    assert.equal(lo, upper, `bucket ${i} starts where ${i - 1} ended`);
    assert.ok(hi > lo);
    if (lo >= 4) assert.ok((hi - lo) / lo <= 0.25, `bucket ${i} width`);
    upper = hi;
  }
  assert.ok(upper >= LatencyHistogram.kBuckets, 'the last bucket reaches 1 s');
  for (const us of [0, 1, 3, 4, 5, 7, 8, 9, 10, 99, 100, 127, 128, 1000, 65535, 65536, 999_999]) {
    const [lo, hi] = logBucketEdges(logBucket(us));
    assert.ok(lo <= us && us < hi, `${us} µs is inside its bucket [${lo}, ${hi})`);
  }
});

test('the log histogram counts the same samples as the fine one', () => {
  const h = new LatencyHistogram();
  const rng = new Rng(7);
  const fine = new Map();
  for (let i = 0; i < 20_000; i++) {
    // log-uniform from 0.5 µs to ~4 s, so some land past 1 s
    const ns = Math.floor(500 * 2 ** (rng.next() * 23));
    h.record(ns);
    const us = Math.floor(ns / 1000);
    if (us < LatencyHistogram.kBuckets) fine.set(logBucket(us), (fine.get(logBucket(us)) ?? 0) + 1);
  }
  h.record(-5); // negative: excluded everywhere
  const lh = h.logHistogram();
  assert.equal(lh.edges_us.length, kLogBuckets + 1);
  assert.equal(lh.count, 20_000);
  assert.ok(lh.over_1s > 0);
  assert.equal(lh.counts.reduce((a, b) => a + b, 0) + lh.over_1s, lh.count);
  for (let i = 0; i < kLogBuckets; i++) assert.equal(lh.counts[i], fine.get(i) ?? 0, `bucket ${i}`);
  // the median from the log buckets lands in the same bucket as the exact one
  const p50 = h.percentileUs(0.5);
  let seen = 0;
  let b = 0;
  while (seen + lh.counts[b] < Math.ceil(lh.count / 2)) seen += lh.counts[b++];
  const [lo, hi] = logBucketEdges(b);
  assert.ok(lo < p50 && p50 <= hi, `p50 ${p50} µs within [${lo}, ${hi}]`);
  h.reset();
  assert.equal(h.logHistogram().counts.reduce((a, c) => a + c, 0), 0);
});
