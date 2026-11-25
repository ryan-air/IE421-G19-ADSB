// Packet parsing, the book, the snapshot parser, and the gateway frame
// format, including agreement with the golden packet that the C++, JS and
// Python codecs are all tested against.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { FrameFlags, FrameKind, TrackTable, decodeFrame, encodeFrameHeader, toAircraft } from '../public/gateway_protocol.mjs';
import { ApplyResult, FeedBook, PacketKind, SnapshotParser, bookDigest, encodeRetransmitRequest, parsePacket } from '../src/feed.mjs';
import { repoRoot } from '../src/gateway.mjs';
import { S1, heartbeat, packet, serializeSnapshot, trackDelete, trackUpdate } from './helpers.mjs';
import { TrackUpdateSize } from '../../protocol/generated/feed_messages.mjs';

test('the golden packet parses like the generated decoders say', () => {
  const raw = readFileSync(join(repoRoot, 'protocol', 'golden', 'packet_v1.bin'));
  const expected = JSON.parse(readFileSync(join(repoRoot, 'protocol', 'golden', 'packet_v1.json'), 'utf8'));
  const p = parsePacket(raw);
  assert.ok(p);
  assert.equal(p.session, expected.header.session);
  assert.equal(p.seq, Number(expected.header.seq));
  assert.equal(p.count, expected.header.count);
  assert.equal(p.kind, PacketKind.kData);
  // An update then a delete of the same aircraft.
  const book = new FeedBook();
  const results = p.blocks.map(([off, len]) => book.apply(raw.subarray(off, off + len)));
  assert.deepEqual(
    results,
    expected.messages.map((m) => (m.name === 'TrackUpdate' ? ApplyResult.kUpdate : ApplyResult.kDelete)),
  );
  const [off] = p.blocks[0];
  assert.equal(raw.readUInt32LE(off + 4), expected.messages[0].fields.icao);
  assert.equal(book.size, 0);
});

test('parsePacket enforces the framing rules', () => {
  const ok = packet(S1, 5, 3);
  assert.equal(parsePacket(ok).blocks.length, 3);
  assert.equal(parsePacket(ok.subarray(0, 19)), null, 'shorter than the header');
  assert.equal(parsePacket(ok.subarray(0, ok.length - 1)), null, 'truncated block');
  assert.equal(parsePacket(Buffer.concat([ok, Buffer.from([1])])), null, 'trailing byte');
  assert.equal(parsePacket(heartbeat(S1, 3)).kind, PacketKind.kHeartbeat);
  assert.equal(parsePacket(heartbeat(S1, 3, true)).kind, PacketKind.kEndOfSession);
  assert.equal(parsePacket(Buffer.concat([heartbeat(S1, 3), Buffer.from([0])])), null, 'heartbeat with a body');
  const bigSeq = heartbeat(S1, 0);
  bigSeq.writeBigUInt64LE(2n ** 53n, 10);
  assert.equal(parsePacket(bigSeq), null, 'seq beyond 2^53 - 1');
});

test('retransmit request encoding', () => {
  const b = encodeRetransmitRequest(S1, 123456789012, 77);
  assert.equal(b.length, 20);
  assert.equal(b.toString('latin1', 0, 10), S1);
  assert.equal(b.readBigUInt64LE(10), 123456789012n);
  assert.equal(b.readUInt16LE(18), 77);
});

test('book: updates, deletes, unknown types, malformed, longer messages', () => {
  const book = new FeedBook();
  assert.equal(book.apply(trackUpdate({ icao: 5, alt: 100 })), ApplyResult.kUpdate);
  assert.equal(book.apply(Buffer.concat([trackUpdate({ icao: 6 }), Buffer.from([9, 9])])), ApplyResult.kUpdate);
  assert.equal(book.tracks.get(6).length, 72, 'appended fields are not stored');
  assert.equal(book.apply(trackDelete(5)), ApplyResult.kDelete);
  assert.equal(book.apply(trackDelete(5)), ApplyResult.kDeleteUnknown);
  assert.equal(book.apply(Buffer.from([0x51, 0])), ApplyResult.kUnknownType);
  assert.equal(book.apply(trackUpdate({ icao: 1 }).subarray(0, 71)), ApplyResult.kMalformed);
  assert.equal(book.apply(Buffer.alloc(0)), ApplyResult.kMalformed);
  assert.equal(book.size, 1);
});

test('book stores copies of message bytes', () => {
  const book = new FeedBook();
  const m = trackUpdate({ icao: 9, alt: 1 });
  book.apply(m);
  m.fill(0);
  assert.equal(book.tracks.get(9).readInt32LE(32), 1);
});

test('digest: order-independent, equals SHA-256 of the sorted wire bytes', () => {
  const a = new FeedBook();
  const b = new FeedBook();
  a.apply(trackUpdate({ icao: 2, alt: 2 }));
  a.apply(trackUpdate({ icao: 1, alt: 1 }));
  b.apply(trackUpdate({ icao: 1, alt: 1 }));
  b.apply(trackUpdate({ icao: 2, alt: 2 }));
  assert.equal(a.hexDigest(), b.hexDigest());
  b.apply(trackUpdate({ icao: 2, alt: 3 }));
  assert.notEqual(a.hexDigest(), b.hexDigest());
  // The empty book: SHA-256 of nothing (the C++ tools print the same).
  assert.equal(new FeedBook().hexDigest(), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(bookDigest(a.tracks), a.hexDigest());
});

function sampleBook(n) {
  const book = new FeedBook();
  for (let i = 0; i < n; i++) book.apply(trackUpdate({ icao: 1000 + i * 3, alt: i }));
  return book;
}

test('snapshot parser: any chunking gives the same result', () => {
  const book = sampleBook(25);
  const bytes = serializeSnapshot(S1, 4242, book);
  for (const chunk of [1, 2, 3, 7, 64, 65, 1000, bytes.length]) {
    const p = new SnapshotParser();
    let st;
    for (let off = 0; off < bytes.length; off += chunk) st = p.feed(bytes.subarray(off, off + chunk));
    assert.equal(st, 'done', `chunk ${chunk}: ${p.error}`);
    assert.equal(p.data.session, S1);
    assert.equal(p.data.asOfSeq, 4242);
    assert.equal(p.data.tracks.length, 25);
    assert.equal(bookDigest(new Map(p.data.tracks.map((t) => [t.readUInt32LE(4), t]))), book.hexDigest());
  }
});

test('snapshot parser: empty book', () => {
  const p = new SnapshotParser();
  assert.equal(p.feed(serializeSnapshot(S1, 0, new FeedBook())), 'done');
  assert.equal(p.data.tracks.length, 0);
});

test('snapshot parser rejects every malformed stream', () => {
  const good = serializeSnapshot(S1, 10, sampleBook(3));
  const run = (bytes) => {
    const p = new SnapshotParser();
    const st = p.feed(bytes);
    return st === 'error' ? p.error : st;
  };
  assert.equal(run(good), 'done');
  assert.match(run(Buffer.concat([good, Buffer.from([0])])), /after SnapshotEnd/);
  assert.equal(run(good.subarray(0, good.length - 1)), 'need-more');
  // Wrong first message type.
  const badType = Buffer.from(good);
  badType[12] = 0x55;
  assert.match(run(badType), /SnapshotBegin/);
  // Track count says 4, only 3 follow, then End.
  const badCount = Buffer.from(good);
  badCount.writeUInt32LE(4, 12 + 4);
  assert.match(run(badCount), /TrackUpdate/);
  // Huge declared count.
  const huge = Buffer.from(good);
  huge.writeUInt32LE(2 ** 30, 12 + 4);
  assert.match(run(huge), /too large/);
  // as_of mismatch between Begin and End.
  const mismatch = Buffer.from(good);
  mismatch.writeBigUInt64LE(11n, good.length - 8);
  assert.match(run(mismatch), /differs/);
  // Tracks out of order.
  const book = new FeedBook();
  book.apply(trackUpdate({ icao: 5 }));
  book.apply(trackUpdate({ icao: 4 }));
  const unsorted = serializeSnapshot(S1, 1, book);
  // serializeSnapshot sorts; swap the two track blocks by hand.
  const blk = 2 + TrackUpdateSize; // u16 length + TrackUpdate
  const a = unsorted.subarray(10 + 26, 10 + 26 + blk);
  const b = unsorted.subarray(10 + 26 + blk, 10 + 26 + 2 * blk);
  const swapped = Buffer.concat([unsorted.subarray(0, 36), b, a, unsorted.subarray(10 + 26 + 2 * blk)]);
  assert.match(run(swapped), /order/);
  // The same ICAO twice (strictly increasing means no repeats).
  const dupBook = new FeedBook();
  dupBook.apply(trackUpdate({ icao: 4 }));
  dupBook.apply(trackUpdate({ icao: 5 }));
  const twoTracks = serializeSnapshot(S1, 1, dupBook);
  const first = twoTracks.subarray(36, 36 + blk);
  const dup = Buffer.concat([twoTracks.subarray(0, 36), first, first, twoTracks.subarray(36 + 2 * blk)]);
  assert.match(run(dup), /order/);
  // Bytes fed after done.
  const p = new SnapshotParser();
  p.feed(good);
  assert.equal(p.feed(Buffer.from([1])), 'error');
});

test('gateway frames round-trip and the table follows them', () => {
  const u1 = trackUpdate({ icao: 0xabcdef, alt: 35000, lat: 40.1, lon: -88.2, callsign: 'UAL123' });
  const u2 = trackUpdate({ icao: 7, alt: 1000 });
  const len = (n) => {
    const b = Buffer.alloc(2);
    b.writeUInt16LE(n);
    return b;
  };
  const snap = Buffer.concat([encodeFrameHeader(FrameKind.kSnapshot, 0, 2, 10), len(72), u1, len(72), u2]);
  const table = new TrackTable();
  assert.equal(table.apply(decodeFrame(snap)), null);
  assert.equal(table.tracks.size, 2);
  assert.equal(table.asOfSeq, 10);
  const delta = Buffer.concat([encodeFrameHeader(FrameKind.kDelta, FrameFlags.kStale, 1, 12), len(16), trackDelete(7)]);
  assert.deepEqual(table.apply(decodeFrame(delta)), [7]);
  assert.equal(table.tracks.size, 1);
  assert.equal(table.flags, FrameFlags.kStale);
  const a = toAircraft(table.tracks.get(0xabcdef), 5);
  assert.equal(a.hex, 'ABCDEF');
  assert.equal(a.flight, 'UAL123');
  assert.equal(a.altitude, 35000);
  assert.ok(Math.abs(a.lat - 40.1) < 1e-6 && Math.abs(a.lon + 88.2) < 1e-6);
  assert.equal(a.speed, null);
  assert.equal(a.track, null);
  assert.equal(a.seen, 5);
  assert.equal(a.heading, null);
  assert.equal(a.heading_ref, null);
  assert.equal(a.airspeed, null);
  assert.equal(a.adsb_version, null);
  assert.doesNotThrow(() => JSON.stringify(a));
  // Feed v1 fields: heading 0.01 deg, airspeed kt, references in flags bits 4/5.
  const v1 = toAircraft(trackUpdate({ icao: 1, lat: 40, lon: -88, nic: 8, version: 2, flags: 16 | 32, heading: 27140, airspeed: 452 }));
  assert.deepEqual([v1.heading, v1.heading_ref, v1.airspeed, v1.airspeed_type, v1.nic, v1.adsb_version],
    [271.4, 'true', 452, 'tas', 8, 2]);
  const v1b = toAircraft(trackUpdate({ icao: 1, nic: 8, heading: 9000, airspeed: 300 }));
  assert.deepEqual([v1b.heading_ref, v1b.airspeed_type, v1b.nic], ['magnetic', 'ias', null], 'no position: no nic');
  assert.equal(v1.air_data_source, 'adsb');
  assert.equal(a.air_data_source, null);
  // Accuracy bytes 68-70: NACp, SIL (+ per-sample bit 2), NUCp; 0 = unknown.
  assert.deepEqual([a.nac_p, a.sil, a.sil_type, a.nuc_p], [null, null, null, null]);
  const acc = toAircraft(trackUpdate({ icao: 1, lat: 40, lon: -88, nacp: 10, sil: 3 | 4, nucp: 7,
    flags: 64, heading: 9000, airspeed: 250 }));
  assert.deepEqual([acc.nac_p, acc.sil, acc.sil_type, acc.nuc_p, acc.air_data_source, acc.heading_ref],
    [10, 3, 'per_sample', 7, 'comm_b', 'magnetic']);
  const accHour = toAircraft(trackUpdate({ icao: 1, nacp: 9, sil: 2, nucp: 7 }));
  assert.deepEqual([accHour.sil, accHour.sil_type, accHour.nuc_p], [2, 'per_hour', null], 'no position: no nuc_p');
  // Malformed frames.
  assert.throws(() => decodeFrame(snap.subarray(0, 15)), RangeError);
  assert.throws(() => decodeFrame(snap.subarray(0, snap.length - 1)), RangeError);
  assert.throws(() => decodeFrame(Buffer.concat([snap, Buffer.from([0])])), RangeError);
  const badKind = Buffer.from(snap);
  badKind[0] = 0x58;
  assert.throws(() => decodeFrame(badKind), RangeError);
});
