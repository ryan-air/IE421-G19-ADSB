// SubscriberCore (JS port): every rule of the C++ scenario tests
// (core/tests/test_feed_subscriber.cpp), then a randomized simulation that
// checks the TLA+ invariants (in-order apply, PrefixOfPublisher) at every
// step and the final book byte for byte.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Mode, SnapshotParser, Source, SubscriberCore } from '../src/feed.mjs';
import { Rng, S1, S2, S3, SimPublisher, heartbeat, msgFor, packet, packetOf, reference, snapshotAsOf, trackDelete, trackUpdate } from './helpers.mjs';

class Fixture {
  constructor(cfg = {}) {
    this.reqs = [];
    this.seqs = [];
    this.resets = 0;
    this.snapshots = 0;
    this.sub = new SubscriberCore(
      cfg,
      { sendRequest: (session, seq, count) => this.reqs.push({ session, seq, count }) },
      {
        onMessage: (seq) => this.seqs.push(seq),
        onReset: () => this.resets++,
        onSnapshotLoaded: () => this.snapshots++,
      },
    );
  }
  feed(d, now = 0) {
    this.sub.onDatagram(d, Source.kFeed, now);
  }
  reply(d, now = 0) {
    this.sub.onDatagram(d, Source.kRetransmit, now);
  }
  bookIs(n) {
    return this.sub.book.hexDigest() === reference(n).hexDigest();
  }
}

const range = (a, b) => Array.from({ length: b - a + 1 }, (_, i) => a + i);

test('applies in order without requests', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 3));
  f.feed(packet(S1, 4, 2));
  f.feed(heartbeat(S1, 6));
  assert.equal(f.sub.expected, 6);
  assert.ok(f.bookIs(5));
  assert.equal(f.reqs.length, 0);
  assert.deepEqual(f.seqs, range(1, 5));
  assert.equal(f.sub.stats.gaps, 0);
});

test('gap is requested, buffered, and filled by retransmit', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 2));
  f.feed(packet(S1, 6, 2));
  assert.deepEqual(f.reqs, [{ session: S1, seq: 3, count: 3 }]);
  assert.equal(f.sub.expected, 3);
  assert.ok(f.bookIs(2));
  f.feed(packet(S1, 8, 1));
  assert.equal(f.reqs.length, 1, 'no second request before the timeout');
  f.reply(packet(S1, 3, 3));
  assert.equal(f.sub.expected, 9);
  assert.ok(f.bookIs(8));
  assert.deepEqual(f.seqs, range(1, 8));
  assert.equal(f.sub.reqActive, false);
  assert.equal(f.sub.stats.gaps, 1);
});

test('duplicates and overlaps are applied once', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 4));
  f.feed(packet(S1, 1, 4));
  f.feed(packet(S1, 3, 4));
  f.reply(packet(S1, 2, 2));
  assert.equal(f.sub.expected, 7);
  assert.deepEqual(f.seqs, range(1, 6));
  assert.equal(f.sub.stats.duplicates, 8);
  f.feed(packet(S1, 9, 1));
  f.feed(packet(S1, 9, 1));
  assert.equal(f.sub.stats.buffered, 1);
});

test('heartbeat reveals a lost tail', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 5));
  f.feed(heartbeat(S1, 9));
  assert.deepEqual(f.reqs, [{ session: S1, seq: 6, count: 3 }]);
});

test('retries after the timeout from where it got', () => {
  const f = new Fixture({ retransmitTimeoutNs: 1000 });
  f.feed(packet(S1, 1, 1), 0);
  f.feed(packet(S1, 10, 1), 0);
  assert.equal(f.reqs.length, 1);
  f.sub.poll(999);
  assert.equal(f.reqs.length, 1);
  f.sub.poll(1000); // no answer at all: retried at the timeout
  assert.equal(f.reqs.length, 2);
  assert.deepEqual(f.reqs[1], { session: S1, seq: 2, count: 8 });
  assert.equal(f.sub.stats.request_retries, 1);
  f.reply(packet(S1, 2, 3), 1500); // a short answer: the rest at once
  assert.equal(f.reqs.length, 3);
  assert.deepEqual(f.reqs[2], { session: S1, seq: 5, count: 5 });
  assert.equal(f.sub.stats.request_retries, 1);
  assert.equal(f.sub.stats.truncated_replies, 1);
});

test('a truncated answer is followed up at once but duplicates are not', () => {
  const f = new Fixture({ maxRequest: 100 });
  f.feed(packet(S1, 1, 1), 0);
  f.feed(heartbeat(S1, 101), 0);
  assert.equal(f.reqs.length, 1);
  f.reply(packet(S1, 2, 22), 10); // full packet
  assert.equal(f.reqs.length, 1);
  f.reply(packet(S1, 24, 10), 20); // short: the server stopped at 33
  assert.equal(f.reqs.length, 2);
  assert.equal(f.reqs[1].seq, 34);
  f.reply(packet(S1, 24, 10), 30); // duplicates of the earlier answer
  f.reply(packet(S1, 2, 22), 30);
  assert.equal(f.reqs.length, 2);
  f.reply(packet(S1, 40, 5), 40); // reordered, ahead of expected
  assert.equal(f.reqs.length, 2);
  assert.equal(f.sub.stats.truncated_replies, 1);
});

test('never requests more than the window holds', () => {
  const f = new Fixture({ window: 8, maxRequest: 256 });
  f.feed(packet(S1, 1, 1));
  f.feed(heartbeat(S1, 1001));
  assert.equal(f.reqs.length, 1);
  assert.equal(f.reqs[0].count, 8);
});

test('large gap is requested in chunks', () => {
  const f = new Fixture({ maxRequest: 100 });
  f.feed(packet(S1, 1, 1));
  f.feed(heartbeat(S1, 1001));
  assert.equal(f.reqs.length, 1);
  assert.equal(f.reqs[0].count, 100);
  // Full reply packets (22 TrackUpdates each), as the server builds them.
  for (let q = 2; q < 102; q += 22) f.reply(packet(S1, q, Math.min(22, 102 - q)));
  assert.equal(f.reqs.length, 2);
  assert.equal(f.reqs[1].seq, 102);
});

test('NAK switches to snapshot, then buffered messages are applied', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 2));
  f.feed(packet(S1, 100, 3));
  assert.equal(f.reqs.length, 1);
  f.reply(heartbeat(S1, 50));
  assert.equal(f.sub.mode, Mode.kSnapshot);
  assert.equal(f.sub.stats.naks, 1);
  assert.equal(f.sub.takeSnapshotRequest(0), true);
  assert.equal(f.sub.takeSnapshotRequest(1), false, 'one fetch at a time');
  f.feed(packet(S1, 103, 2));
  f.sub.onSnapshot(snapshotAsOf(S1, 101), 2);
  assert.equal(f.sub.mode, Mode.kLive);
  assert.equal(f.sub.expected, 105);
  assert.ok(f.bookIs(104));
  assert.equal(f.snapshots, 1);
});

test('stale NAK is ignored', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 2));
  f.feed(packet(S1, 10, 1));
  f.reply(packet(S1, 3, 7));
  f.reply(heartbeat(S1, 5));
  assert.equal(f.sub.mode, Mode.kLive);
  assert.equal(f.sub.stats.naks, 0);
  assert.equal(f.sub.expected, 11);
});

test('snapshot older than the current state keeps the state', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 10));
  f.feed(packet(S1, 200, 1));
  f.reply(heartbeat(S1, 150));
  assert.equal(f.sub.mode, Mode.kSnapshot);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S1, 5), 0);
  assert.equal(f.sub.mode, Mode.kLive);
  assert.equal(f.sub.expected, 11);
  assert.ok(f.bookIs(10));
  assert.equal(f.snapshots, 0);
  assert.equal(f.reqs.at(-1).seq, 11);
});

test('failed snapshot is retried after the retry interval', () => {
  const f = new Fixture({ snapshotRetryNs: 1000 });
  f.feed(packet(S1, 50, 1));
  assert.ok(f.sub.takeSnapshotRequest(10));
  f.sub.onSnapshotFailed(20);
  assert.equal(f.sub.takeSnapshotRequest(1019), false);
  assert.equal(f.sub.takeSnapshotRequest(1020), true);
  assert.equal(f.sub.stats.snapshot_failures, 1);
  assert.equal(f.sub.stats.snapshot_requests, 2);
});

test('join at the start needs no snapshot; a late join takes one', () => {
  const early = new Fixture();
  early.feed(heartbeat(S1, 1));
  assert.equal(early.sub.mode, Mode.kLive);
  assert.equal(early.sub.takeSnapshotRequest(0), false);

  const second = new Fixture();
  second.feed(packet(S1, 2, 1));
  assert.equal(second.sub.mode, Mode.kSnapshot);

  const late = new Fixture();
  late.feed(packet(S1, 5000, 2));
  assert.equal(late.sub.mode, Mode.kSnapshot);
  assert.equal(late.reqs.length, 0);
  assert.ok(late.sub.takeSnapshotRequest(0));
  late.feed(packet(S1, 5002, 1));
  late.sub.onSnapshot(snapshotAsOf(S1, 4999), 0);
  assert.equal(late.sub.expected, 5003);
  assert.ok(late.bookIs(5002));
});

test('late join buffers far ahead of expected', () => {
  const f = new Fixture({ window: 64 });
  f.feed(packet(S1, 1_000_000, 10));
  assert.equal(f.sub.mode, Mode.kSnapshot);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.feed(packet(S1, 1_000_010, 10));
  f.sub.onSnapshot(snapshotAsOf(S1, 1_000_004), 0);
  assert.equal(f.sub.expected, 1_000_020);
  assert.equal(f.sub.stats.window_overflow, 0);
});

test('publisher restart clears the book and drops stale packets', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 20));
  assert.equal(f.sub.book.size, 7);
  f.feed(packet(S2, 1, 3));
  assert.equal(f.sub.session, S2);
  assert.equal(f.sub.mode, Mode.kLive);
  assert.equal(f.sub.expected, 4);
  assert.ok(f.bookIs(3));
  assert.equal(f.resets, 2);
  assert.ok(f.sub.confirming, 'a session change is confirmed with the server, in the background');
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S2, 2), 0);
  assert.equal(f.sub.confirming, false);
  assert.equal(f.sub.expected, 4, 'a confirmation does not replace our state');
  assert.ok(f.bookIs(3));
  assert.equal(f.sub.stats.sessions_confirmed, 1);
  assert.equal(f.snapshots, 0);
  f.feed(packet(S1, 21, 5));
  f.reply(packet(S1, 21, 5));
  assert.equal(f.sub.session, S2);
  assert.equal(f.sub.expected, 4);
  assert.equal(f.sub.stats.stale_packets, 2);
  assert.equal(f.sub.stats.session_changes, 1);
});

test('a retransmit reply from a session never seen is ignored', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 3));
  f.reply(packet(S2, 1, 5)); // replies never introduce a session
  f.reply(heartbeat(S2, 50));
  assert.equal(f.sub.session, S1);
  assert.equal(f.sub.expected, 4);
  assert.equal(f.sub.stats.stale_packets, 2);
  assert.equal(f.sub.stats.session_changes, 0);
  assert.ok(f.bookIs(3));
});

test('restart seen mid-session takes a snapshot of the new session', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 5));
  f.feed(packet(S2, 30, 1));
  assert.equal(f.sub.mode, Mode.kSnapshot);
  assert.equal(f.sub.book.size, 0);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S2, 29), 0);
  assert.equal(f.sub.expected, 31);
  assert.ok(f.bookIs(30));
});

test('snapshot from an even newer session is adopted', () => {
  const f = new Fixture();
  f.feed(packet(S2, 30, 1));
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S3, 7), 0);
  assert.equal(f.sub.session, S3);
  assert.equal(f.sub.expected, 8);
  assert.ok(f.bookIs(7));
  f.feed(packet(S2, 31, 1));
  assert.equal(f.sub.stats.stale_packets, 1);
});

test('packet from two sessions ago is stale', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 3));
  f.feed(packet(S2, 1, 3));
  f.feed(packet(S3, 1, 3));
  assert.ok(f.sub.takeSnapshotRequest(0)); // confirms the change to S3
  f.sub.onSnapshot(snapshotAsOf(S3, 3), 0);
  f.feed(packet(S1, 4, 1));
  f.feed(packet(S2, 4, 1));
  assert.equal(f.sub.session, S3);
  assert.equal(f.sub.mode, Mode.kLive);
  assert.equal(f.sub.stats.stale_packets, 2);
  f.feed(packet(S3, 4, 1));
  assert.equal(f.sub.expected, 5);
});

// Found by deterministic simulation of the C++ SubscriberCore (adsb-dst seed
// 71): a late packet of the old session, arriving after the new session's
// first packets, must not leave the subscriber on the dead session.
test("a late old-session packet after a restart is undone by the server's snapshot", () => {
  const f = new Fixture();
  f.feed(packet(S2, 1, 3));
  assert.equal(f.sub.mode, Mode.kLive);
  assert.equal(f.sub.stats.snapshot_requests, 0);
  f.feed(heartbeat(S1, 1, true));
  assert.equal(f.sub.session, S1);
  assert.ok(f.sub.confirming, 'a session change must be confirmed');
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S2, 3), 0);
  assert.equal(f.sub.session, S2);
  assert.equal(f.sub.mode, Mode.kLive);
  f.feed(packet(S2, 4, 2));
  assert.equal(f.sub.expected, 6);
  assert.ok(f.bookIs(5));
  f.feed(heartbeat(S1, 1, true));
  assert.equal(f.sub.session, S2, 'S1 is retired now');
});

test('an unconfirmed session change still recovers and ends', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 3));
  f.feed(packet(S2, 1, 2));
  assert.ok(f.sub.confirming);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshotFailed(0);
  f.feed(packet(S2, 5, 1), 1);
  assert.equal(f.reqs.length, 1, 'retransmit requests go out while unconfirmed');
  assert.equal(f.reqs[0].seq, 3);
  f.reply(packet(S2, 3, 2), 2);
  f.feed(heartbeat(S2, 6, true), 3);
  assert.ok(f.sub.ended, 'the end is reached without the confirmation');
  assert.ok(f.bookIs(5));
  assert.ok(f.sub.confirming);
  assert.equal(f.sub.takeSnapshotRequest(1), false);
});

test("stray unknown session is undone by the server's snapshot", () => {
  const junk = 'JUNK000001';
  const f = new Fixture();
  f.feed(packet(S1, 1, 3));
  f.feed(packet(S2, 1, 3));
  f.feed(packet(junk, 77, 1));
  assert.equal(f.sub.session, junk);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S2, 3), 0);
  assert.equal(f.sub.session, S2);
  assert.equal(f.sub.mode, Mode.kLive);
  f.feed(packet(S2, 4, 2));
  assert.equal(f.sub.expected, 6);
  assert.ok(f.bookIs(5));
  f.feed(packet(junk, 78, 1));
  assert.equal(f.sub.session, S2);
});

test('a snapshot requested before a session change is never adopted', () => {
  const f = new Fixture();
  f.feed(packet(S1, 50, 1));
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.feed(packet(S3, 1, 2));
  assert.equal(f.sub.session, S3);
  f.sub.onSnapshot(snapshotAsOf(S2, 9), 0);
  assert.equal(f.sub.session, S3, 'never adopted: it was asked for in S1');
  assert.equal(f.sub.expected, 3);
  assert.equal(f.sub.stats.snapshot_failures, 1);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S3, 2), 0);
  assert.equal(f.sub.confirming, false);
  assert.equal(f.sub.session, S3);
});

// Found by deterministic simulation of the C++ SubscriberCore (adsb-dst seed
// 40013996): S1 -> S3 (newest packets first) -> S2 (a late packet of the
// session between); the S1-era fetch, answered by S2's dead server, matches
// the current session but is stale.
test('a stale answer is rejected even when it matches the current session', () => {
  const f = new Fixture();
  f.feed(packet(S1, 50, 1));
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.feed(heartbeat(S3, 2, true));
  assert.equal(f.sub.session, S3);
  f.feed(packet(S2, 2, 1));
  assert.equal(f.sub.session, S2);
  f.sub.onSnapshot(snapshotAsOf(S2, 4), 0);
  assert.equal(f.sub.mode, Mode.kSnapshot, 'not loaded: asked for two session changes ago');
  assert.equal(f.sub.stats.snapshot_failures, 1);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S3, 1), 0);
  assert.equal(f.sub.session, S3);
  assert.equal(f.sub.mode, Mode.kLive);
  f.feed(heartbeat(S3, 2, true));
  assert.ok(f.sub.ended);
  assert.equal(f.sub.book.hexDigest(), reference(1).hexDigest());
});

test('snapshot requested before a restart is rejected', () => {
  const f = new Fixture();
  f.feed(packet(S1, 50, 1));
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.feed(packet(S2, 20, 1));
  f.sub.onSnapshot(snapshotAsOf(S1, 49), 0);
  assert.equal(f.sub.session, S2);
  assert.equal(f.sub.mode, Mode.kSnapshot);
  assert.equal(f.sub.stats.snapshot_failures, 1);
});

test('boundaries: NAK at expected, snapshot at expected - 1', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 4));
  f.feed(packet(S1, 9, 1));
  f.reply(heartbeat(S1, 5));
  assert.equal(f.sub.mode, Mode.kLive);
  f.reply(heartbeat(S1, 6));
  assert.equal(f.sub.mode, Mode.kSnapshot);
  assert.ok(f.sub.takeSnapshotRequest(0));
  f.sub.onSnapshot(snapshotAsOf(S1, 3), 0);
  assert.equal(f.snapshots, 0);
  assert.equal(f.sub.expected, 5);
  f.reply(heartbeat(S1, 6));
  assert.ok(f.sub.takeSnapshotRequest(1e9));
  f.sub.onSnapshot(snapshotAsOf(S1, 4), 0);
  assert.equal(f.snapshots, 1);
  assert.equal(f.sub.expected, 5);
  assert.ok(f.bookIs(4));
});

test('rejects forged sequence jumps and seq 0 and seq beyond 2^53', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 2));
  f.feed(heartbeat(S1, 2 ** 40));
  f.feed(heartbeat(S1, 0));
  const huge = heartbeat(S1, 0);
  huge.writeBigUInt64LE(2n ** 60n, 10);
  f.feed(huge);
  assert.equal(f.sub.stats.malformed, 3);
  assert.equal(f.sub.knownNext, 3);
  assert.equal(f.reqs.length, 0);
});

test('oversized window is clamped', () => {
  const sub = new SubscriberCore({ window: 3e9 });
  assert.equal(sub.cfg.window, 1 << 20);
  sub.onDatagram(packet(S1, 1, 1), Source.kFeed, 0);
  assert.equal(sub.expected, 2);
});

test('end of session waits for the tail', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 5));
  f.feed(heartbeat(S1, 9, true));
  assert.equal(f.sub.ended, false);
  assert.equal(f.reqs.length, 1);
  f.reply(packet(S1, 6, 3));
  assert.equal(f.sub.ended, true);
});

test('malformed datagrams are counted and ignored', () => {
  const f = new Fixture();
  const d = packet(S1, 1, 2);
  f.feed(d.subarray(0, d.length - 1));
  f.feed(Buffer.alloc(5));
  const withBody = Buffer.concat([heartbeat(S1, 1), Buffer.from([0])]);
  f.feed(withBody);
  const trailing = Buffer.concat([packet(S1, 1, 1), Buffer.from([0])]);
  f.feed(trailing);
  assert.equal(f.sub.stats.malformed, 4);
  assert.equal(f.sub.hasSession, false);
});

test('window overflow is recovered later', () => {
  const f = new Fixture({ window: 16, maxRequest: 8 });
  f.feed(packet(S1, 1, 1));
  f.feed(packet(S1, 40, 1));
  assert.equal(f.sub.stats.window_overflow, 1);
  for (let round = 0; round < 10 && f.sub.expected <= 40; round++) {
    const r = f.reqs.at(-1);
    f.reply(packet(S1, r.seq, r.count));
  }
  assert.equal(f.sub.expected, 41);
  assert.ok(f.bookIs(40));
});

test('unknown message types are skipped but sequenced', () => {
  const f = new Fixture();
  f.feed(packetOf(S1, 1, [msgFor(1), Buffer.from([0x51, 0, 1, 2, 3])]));
  assert.equal(f.sub.expected, 3);
  assert.equal(f.sub.stats.unknown_messages, 1);
  assert.equal(f.sub.book.size, 1);
});

test('buffered messages are copies, not views into reused datagrams', () => {
  const f = new Fixture();
  f.feed(packet(S1, 1, 1));
  const d = packet(S1, 3, 1);
  f.feed(d);
  d.fill(0); // the socket layer may reuse the buffer
  f.reply(packet(S1, 2, 1));
  assert.ok(f.bookIs(3));
});

// ---------------------------------------------------------------------------
// Randomized simulation
// ---------------------------------------------------------------------------

function simulate(p) {
  const params = {
    seed: 1,
    messages: 2000,
    drop: 0.1,
    dup: 0.05,
    reqDrop: 0.1,
    replyDrop: 0.1,
    snapFail: 0.2,
    ring: 256,
    window: 128,
    delay: 0.02,
    outage: false,
    restart: false,
    lateJoin: false,
    ...p,
  };
  const rng = new Rng(params.seed);
  let now = 1;
  const net = { feed: [], replies: [], requests: [], delayed: [] };
  let lossy = true;

  let violations = 0;
  let last = 0;
  let fresh = true;
  const sub = new SubscriberCore(
    { window: params.window, maxRequest: 64, retransmitTimeoutNs: 50, snapshotRetryNs: 200 },
    {
      sendRequest: (session, seq, count) => {
        const b = Buffer.alloc(20);
        b.write(session, 0, 10, 'latin1');
        b.writeBigUInt64LE(BigInt(seq), 10);
        b.writeUInt16LE(count, 18);
        net.requests.push(b);
      },
    },
    {
      onMessage: (seq) => {
        if (!fresh && seq !== last + 1) violations++;
        fresh = false;
        last = seq;
      },
      onReset: () => {
        fresh = true;
      },
      onSnapshotLoaded: () => {
        fresh = true;
      },
    },
  );

  let pub = new SimPublisher(S1, { ring: params.ring, maxReply: 40, trackDigests: true });
  const restartAt = params.restart ? rng.below(params.messages) : -1;
  const outageAt = params.outage ? rng.below(params.messages / 2) : -1;
  const joinAt = params.lateJoin ? params.messages / 4 + rng.below(params.messages / 4) : 0;
  let published = 0;
  let pendingSnap = null;
  let prefixViolations = 0;
  let checkedExpected = 0;
  let checkedPub = null;
  let lastHeartbeat = 0;

  const deliverOne = (q, src, joined, deaf) => {
    if (q.length === 0) return;
    const i = rng.below(Math.min(q.length, 4));
    const d = q[i];
    const drop = src === Source.kFeed ? params.drop : params.replyDrop;
    const dup = lossy && rng.chance(params.dup);
    if (!dup) q.splice(i, 1);
    if (lossy && rng.chance(drop)) return;
    if ((!joined || deaf) && src === Source.kFeed) return;
    if (lossy && rng.chance(params.delay)) {
      net.delayed.push({ at: now + 1 + rng.below(200), src, d });
      return;
    }
    sub.onDatagram(d, src, now);
  };

  let step = 0;
  for (; step < 400_000; step++) {
    now++;
    const joined = published >= joinAt;
    if (published < params.messages && rng.chance(0.6)) {
      if (published === restartAt) {
        const tail = pub.flush();
        if (tail) net.feed.push(tail);
        net.feed.push(pub.heartbeat(true));
        pub = new SimPublisher(S2, { ring: params.ring, maxReply: 40, trackDigests: true });
      }
      const icao = rng.below(40) + 1;
      pub.publish(
        rng.chance(0.15)
          ? trackDelete(icao, now)
          : trackUpdate({ icao, alt: rng.below(40000), eventTs: now, publishTs: published }),
      );
      published++;
      if (rng.chance(0.5) || published === params.messages) {
        const pkt = pub.flush();
        if (pkt) net.feed.push(pkt);
      }
      if (published === params.messages) {
        lossy = false; // loss eventually ceases (TLA+ fairness)
        net.feed.push(pub.heartbeat(true));
      }
    }
    if (now - lastHeartbeat >= 20) {
      lastHeartbeat = now;
      net.feed.push(pub.heartbeat(published === params.messages));
    }

    const deaf = outageAt >= 0 && published >= outageAt && published < outageAt + 600;
    deliverOne(net.feed, Source.kFeed, joined, deaf);
    net.delayed.sort((a, b) => a.at - b.at);
    while (net.delayed.length && net.delayed[0].at <= now) {
      const { src, d } = net.delayed.shift();
      sub.onDatagram(d, src, now);
    }
    if (net.requests.length) {
      const r = net.requests.shift();
      if (!(lossy && rng.chance(params.reqDrop))) net.replies.push(...pub.handleRequest(r));
    }
    deliverOne(net.replies, Source.kRetransmit, joined, false);

    sub.poll(now);
    if (!pendingSnap && sub.takeSnapshotRequest(now)) {
      if (lossy && rng.chance(params.snapFail)) sub.onSnapshotFailed(now);
      else {
        const parser = new SnapshotParser();
        // Feed it in random chunks: the parser must not care.
        const bytes = pub.snapshotBytes();
        let st;
        for (let off = 0; off < bytes.length; ) {
          const n = 1 + rng.below(200);
          st = parser.feed(bytes.subarray(off, off + n));
          off += n;
        }
        assert.equal(st, 'done', parser.error);
        pendingSnap = { data: parser.data, at: now + rng.below(30) };
      }
    }
    if (pendingSnap && now >= pendingSnap.at) {
      sub.onSnapshot(pendingSnap.data, now);
      pendingSnap = null;
    }

    if (
      sub.hasSession &&
      sub.session === pub.session &&
      sub.mode === Mode.kLive &&
      (sub.expected !== checkedExpected || checkedPub !== pub)
    ) {
      checkedExpected = sub.expected;
      checkedPub = pub;
      const applied = sub.expected - 1;
      if (applied >= pub.digestAt.length || sub.book.hexDigest() !== pub.digestAt[applied]) prefixViolations++;
    }
    if (published === params.messages && sub.ended && sub.session === pub.session && net.delayed.length === 0) break;
  }

  const ctx = `seed ${params.seed}`;
  assert.equal(violations, 0, `${ctx}: out-of-order apply`);
  assert.equal(prefixViolations, 0, `${ctx}: book diverged from the publisher`);
  assert.ok(sub.ended, `${ctx}: did not converge (expected ${sub.expected}, known ${sub.knownNext})`);
  assert.equal(sub.session, pub.session, ctx);
  assert.equal(sub.book.hexDigest(), pub.book.hexDigest(), `${ctx}: final book differs`);
  return sub.stats;
}

test('simulation converges byte for byte under loss, duplication and reordering', () => {
  const total = { requests: 0, naks: 0, snapshots_loaded: 0, duplicates: 0, request_retries: 0 };
  for (let seed = 1; seed <= 30; seed++) {
    const s = simulate({ seed, outage: seed % 2 === 0 });
    for (const k of Object.keys(total)) total[k] += s[k];
  }
  // Non-vacuity: every recovery path was exercised.
  assert.ok(total.requests > 100, JSON.stringify(total));
  assert.ok(total.request_retries > 10, JSON.stringify(total));
  assert.ok(total.naks > 5, JSON.stringify(total));
  assert.ok(total.snapshots_loaded > 5, JSON.stringify(total));
  assert.ok(total.duplicates > 100, JSON.stringify(total));
});

test('simulation converges across a publisher restart', () => {
  let changes = 0;
  let stale = 0;
  for (let seed = 100; seed < 115; seed++) {
    const s = simulate({ seed, delay: 0.1, restart: true });
    changes += s.session_changes;
    stale += s.stale_packets;
  }
  assert.equal(changes, 15);
  assert.ok(stale > 0, 'reordering delivered old-session packets after the switch');
});

test('simulation: late joiners converge', () => {
  for (let seed = 200; seed < 212; seed++) {
    const s = simulate({ seed, lateJoin: true });
    assert.ok(s.snapshots_loaded >= 1, `seed ${seed}`);
  }
});

test('simulation: heavy loss with a large ring never needs a NAK', () => {
  for (let seed = 300; seed < 308; seed++) {
    const s = simulate({ seed, drop: 0.3, ring: 1 << 16, window: 1 << 14 });
    assert.equal(s.naks, 0, `seed ${seed}`);
    assert.ok(s.snapshots_loaded <= 1, `seed ${seed}`);
  }
});
