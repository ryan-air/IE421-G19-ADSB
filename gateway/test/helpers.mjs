// Test helpers: feed packet builders and a small in-memory publisher with a
// retransmit ring and snapshot server (the JS stand-in for the C++
// Publisher + RecoveryState, used by the simulation).

import { FeedBook } from '../src/feed.mjs';

export const S1 = 'SESSION001';
export const S2 = 'SESSION002';
export const S3 = 'SESSION003';

// A TrackUpdate (72 bytes) with the given fields; the rest are zero or null.
export function trackUpdate({ icao, alt = 0, eventTs = 0, publishTs = 0, lat = null, lon = null, mask = 0, callsign = '',
  flags = 0, nic = 0, version = 0xff, heading = 0xffff, airspeed = 0xffff, nacp = 0, sil = 0, nucp = 0,
  gsDkt = 0xffff, trackCdeg = 0xffff, vrateFpm = -32768 }) {
  const b = Buffer.alloc(72);
  b[0] = 0x55;
  b.writeUInt16LE(mask, 2);
  b.writeUInt32LE(icao, 4);
  b.writeBigUInt64LE(BigInt(eventTs), 8);
  b.writeInt32LE(lat === null ? -2147483648 : Math.round(lat * 1e7), 24);
  b.writeInt32LE(lon === null ? -2147483648 : Math.round(lon * 1e7), 28);
  b.writeInt32LE(alt, 32);
  b.writeUInt16LE(gsDkt, 36);
  b.writeUInt16LE(trackCdeg, 38);
  b.writeInt16LE(vrateFpm, 40);
  b.writeUInt16LE(0xffff, 42);
  b.write(callsign.padEnd(8, ' '), 44, 8, 'latin1');
  b[53] = nic;
  b[54] = flags;
  b[55] = version;
  b.writeBigUInt64LE(BigInt(publishTs), 56);
  b.writeUInt16LE(heading, 64);
  b.writeUInt16LE(airspeed, 66);
  b[68] = nacp;
  b[69] = sil;
  b[70] = nucp;
  return b;
}

export function trackDelete(icao, eventTs = 0) {
  const b = Buffer.alloc(16);
  b[0] = 0x44;
  b[2] = 1;
  b.writeUInt32LE(icao, 4);
  b.writeBigUInt64LE(BigInt(eventTs), 8);
  return b;
}

// Message `seq` updates track (seq % 7) + 1 with altitude = seq (as the C++ tests).
export const msgFor = (seq) => trackUpdate({ icao: (seq % 7) + 1, alt: seq, eventTs: seq });

export function packetOf(session, first, msgs) {
  let size = 20;
  for (const m of msgs) size += 2 + m.length;
  const b = Buffer.alloc(size);
  b.write(session, 0, 10, 'latin1');
  b.writeBigUInt64LE(BigInt(first), 10);
  b.writeUInt16LE(msgs.length, 18);
  let off = 20;
  for (const m of msgs) {
    b.writeUInt16LE(m.length, off);
    m.copy(b, off + 2);
    off += 2 + m.length;
  }
  return b;
}

export function packet(session, first, count) {
  const msgs = [];
  for (let q = first; q < first + count; q++) msgs.push(msgFor(q));
  return packetOf(session, first, msgs);
}

export function heartbeat(session, next, eos = false) {
  const b = Buffer.alloc(20);
  b.write(session, 0, 10, 'latin1');
  b.writeBigUInt64LE(BigInt(next), 10);
  b.writeUInt16LE(eos ? 0xffff : 0, 18);
  return b;
}

export function reference(n) {
  const book = new FeedBook();
  for (let q = 1; q <= n; q++) book.apply(msgFor(q));
  return book;
}

export function snapshotAsOf(session, n) {
  const book = reference(n);
  return { session, asOfSeq: n, tracks: book.sortedIcaos().map((i) => book.tracks.get(i)) };
}

// The snapshot stream bytes for a book (feed_spec.md §3).
export function serializeSnapshot(session, asOfSeq, book) {
  const parts = [Buffer.from(session.padEnd(10, '\0'), 'latin1')];
  const begin = Buffer.alloc(24);
  begin[0] = 0x53;
  begin.writeUInt32LE(book.size, 4);
  begin.writeBigUInt64LE(BigInt(asOfSeq), 8);
  const end = Buffer.alloc(16);
  end[0] = 0x45;
  end.writeBigUInt64LE(BigInt(asOfSeq), 8);
  const block = (m) => {
    const l = Buffer.alloc(2);
    l.writeUInt16LE(m.length, 0);
    parts.push(l, m);
  };
  block(begin);
  for (const icao of book.sortedIcaos()) block(book.tracks.get(icao));
  block(end);
  return Buffer.concat(parts);
}

// Deterministic PRNG (splitmix-like on 32 bits; tests only).
export class Rng {
  constructor(seed) {
    this.s = (seed * 0x9e3779b1) >>> 0 || 1;
  }
  next() {
    let x = (this.s = (this.s + 0x6d2b79f5) >>> 0);
    x = Math.imul(x ^ (x >>> 15), x | 1);
    x ^= x + Math.imul(x ^ (x >>> 7), x | 61);
    return ((x ^ (x >>> 14)) >>> 0) / 2 ** 32;
  }
  below(n) {
    return Math.floor(this.next() * n);
  }
  chance(p) {
    return this.next() < p;
  }
}

// In-memory publisher + recovery server, as RecoveryState::handle_request:
// data replies capped at maxReply, NAK (header-only, seq = oldest held) when
// the request starts before the ring, nothing when it is not published yet.
export class SimPublisher {
  constructor(session, { ring = 256, maxReply = 40, trackDigests = false } = {}) {
    this.session = session;
    this.ring = ring;
    this.maxReply = maxReply;
    this.log = new Map(); // seq -> message (only the last `ring`)
    this.next = 1;
    this.book = new FeedBook(); // after every published message (including unsent ones)
    this.sentBook = new FeedBook(); // after every sent message: what the snapshot server serves
    // digestAt[s] = digest after message s: hashing the whole book per message
    // is O(n) each, so only the simulation (small books) turns it on.
    this.trackDigests = trackDigests;
    this.digestAt = [this.book.hexDigest()];
    this.pending = [];
  }

  publish(msg) {
    this.pending.push(msg);
    this.book.apply(msg);
    if (this.trackDigests) this.digestAt.push(this.book.hexDigest());
  }

  // Sends what is pending as one packet (returns null if nothing is).
  flush() {
    if (this.pending.length === 0) return null;
    const first = this.next;
    const pkt = packetOf(this.session, first, this.pending);
    for (const m of this.pending) {
      this.sentBook.apply(m);
      this.log.set(this.next, m);
      this.log.delete(this.next - this.ring);
      this.next++;
    }
    this.pending = [];
    return pkt;
  }

  heartbeat(eos = false) {
    return heartbeat(this.session, this.next, eos);
  }

  oldest() {
    return Math.max(1, this.next - this.ring);
  }

  handleRequest(dgram) {
    if (dgram.length !== 20 || dgram.toString('latin1', 0, 10) !== this.session) return [];
    const seq = Number(dgram.readBigUInt64LE(10));
    const count = dgram.readUInt16LE(18);
    if (seq < this.oldest()) return [heartbeat(this.session, this.oldest())];
    if (seq >= this.next || count === 0) return [];
    const end = Math.min(seq + count, this.next, seq + this.maxReply);
    const out = [];
    for (let s = seq; s < end; s += 20) {
      const msgs = [];
      for (let q = s; q < Math.min(end, s + 20); q++) msgs.push(this.log.get(q));
      out.push(packetOf(this.session, s, msgs));
    }
    return out;
  }

  snapshotBytes() {
    return serializeSnapshot(this.session, this.next - 1, this.sentBook);
  }
}
