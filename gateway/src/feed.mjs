// Feed protocol for the gateway: packet parsing, the track book, the snapshot
// stream parser, and SubscriberCore, a port of core/src/feed/subscriber.cpp
// (protocol/feed_spec.md §2–4, model-checked in spec/FeedRecovery.tla).
//
// Like the C++ core, nothing here touches sockets or reads a clock: the
// caller passes `now` (nanoseconds, as a Number) so tests can drive it
// deterministically. Sequence numbers are Numbers: exact up to 2^53, which a
// session cannot reach (a packet claiming more is rejected as malformed).
//
// The book keeps raw wire bytes (64-byte TrackUpdates), so the gateway can
// forward them to browsers without re-encoding, and its digest equals the C++
// FeedBook::digest() of the same state (SHA-256 over the tracks in ICAO order).

import { createHash } from 'node:crypto';

import {
  PacketHeaderSize,
  RetransmitRequestSize,
  SnapshotBeginSize,
  SnapshotEndSize,
  TrackDeleteSize,
  TrackUpdateSize,
  kEndOfSessionCount,
  kHeartbeatCount,
  kMaxUdpPayload,
  kSessionLength,
} from '../../protocol/generated/feed_messages.mjs';

export const kMaxMessageBytes = 64; // same as the C++ publisher's log entries
const kUpdate = 0x55; // 'U'
const kDelete = 0x44; // 'D'
const kSnapBegin = 0x53; // 'S'
const kSnapEnd = 0x45; // 'E'

// Largest jump ahead of `expected` a packet may claim (subscriber.cpp).
const kMaxSeqJump = 2 ** 32;

export const PacketKind = Object.freeze({ kData: 0, kHeartbeat: 1, kEndOfSession: 2 });

const u64 = (buf, off) => {
  const v = buf.readBigUInt64LE(off);
  return v > BigInt(Number.MAX_SAFE_INTEGER) ? -1 : Number(v);
};

// Parses one datagram with the rules of moldudp64.hpp parse_packet(): blocks
// must not run past the end, no trailing bytes, no body on heartbeat / end of
// session. Returns null when malformed. `blocks` holds [offset, length] pairs
// into `buf` (no copies).
export function parsePacket(buf) {
  if (buf.length < PacketHeaderSize) return null;
  const session = buf.toString('latin1', 0, kSessionLength);
  const seq = u64(buf, 10);
  const count = buf.readUInt16LE(18);
  if (seq < 0) return null;
  if (count === kHeartbeatCount || count === kEndOfSessionCount) {
    if (buf.length !== PacketHeaderSize) return null;
    return {
      session,
      seq,
      count,
      kind: count === kHeartbeatCount ? PacketKind.kHeartbeat : PacketKind.kEndOfSession,
      blocks: [],
    };
  }
  const blocks = new Array(count);
  let off = PacketHeaderSize;
  for (let i = 0; i < count; i++) {
    if (off + 2 > buf.length) return null;
    const len = buf.readUInt16LE(off);
    off += 2;
    if (off + len > buf.length) return null;
    blocks[i] = [off, len];
    off += len;
  }
  if (off !== buf.length) return null;
  return { session, seq, count, kind: PacketKind.kData, blocks };
}

export function encodeRetransmitRequest(session, seq, count) {
  const b = Buffer.alloc(RetransmitRequestSize);
  b.write(session, 0, kSessionLength, 'latin1');
  b.writeBigUInt64LE(BigInt(seq), 10);
  b.writeUInt16LE(count, 18);
  return b;
}

// ---------------------------------------------------------------------------
// FeedBook
// ---------------------------------------------------------------------------

export const ApplyResult = Object.freeze({
  kUpdate: 0,
  kDelete: 1,
  kDeleteUnknown: 2,
  kUnknownType: 3,
  kMalformed: 4,
});

export class FeedBook {
  constructor() {
    this.tracks = new Map(); // icao -> Buffer (64 bytes, a private copy)
  }

  get size() {
    return this.tracks.size;
  }

  // Applies one message. `msg` may be a view into a datagram: updates are copied.
  apply(msg) {
    if (msg.length === 0) return ApplyResult.kMalformed;
    switch (msg[0]) {
      case kUpdate: {
        // Longer is allowed: later versions may only append fields.
        if (msg.length < TrackUpdateSize) return ApplyResult.kMalformed;
        const icao = msg.readUInt32LE(4);
        this.tracks.set(icao, Buffer.from(msg.subarray(0, TrackUpdateSize)));
        return ApplyResult.kUpdate;
      }
      case kDelete: {
        if (msg.length < TrackDeleteSize) return ApplyResult.kMalformed;
        return this.tracks.delete(msg.readUInt32LE(4)) ? ApplyResult.kDelete : ApplyResult.kDeleteUnknown;
      }
      default:
        return ApplyResult.kUnknownType;
    }
  }

  put(update) {
    this.tracks.set(update.readUInt32LE(4), update);
  }

  clear() {
    this.tracks.clear();
  }

  sortedIcaos() {
    return [...this.tracks.keys()].sort((a, b) => a - b);
  }

  // Hex SHA-256 over every track's 64 wire bytes in ICAO order, as
  // FeedBook::hex_digest() in C++.
  hexDigest() {
    return bookDigest(this.tracks);
  }
}

export function bookDigest(tracks) {
  const h = createHash('sha256');
  for (const icao of [...tracks.keys()].sort((a, b) => a - b)) h.update(tracks.get(icao));
  return h.digest('hex');
}

// ---------------------------------------------------------------------------
// Snapshot stream (feed_spec.md §3): session char[10], then u16-length-prefixed
// SnapshotBegin, TrackUpdate x track_count (strictly increasing ICAO),
// SnapshotEnd. Validated as strictly as snapshot.cpp does.
// ---------------------------------------------------------------------------

export class SnapshotParser {
  static kMaxTracks = 1 << 20;

  constructor() {
    this.state = 'need-more'; // 'need-more' | 'done' | 'error'
    this.error = '';
    this.stage = 'session';
    this.pending = Buffer.alloc(0);
    this.trackCount = 0;
    this.data = { session: '', asOfSeq: 0, eventTsNs: 0n, tracks: [] };
  }

  fail(why) {
    this.state = 'error';
    this.error = why;
    return this.state;
  }

  feed(chunk) {
    if (this.state !== 'need-more') {
      if (this.state === 'done' && chunk.length > 0) return this.fail('bytes after SnapshotEnd');
      return this.state;
    }
    let buf = this.pending.length ? Buffer.concat([this.pending, chunk]) : chunk;
    let off = 0;
    for (;;) {
      if (this.stage === 'session') {
        if (buf.length - off < kSessionLength) break;
        this.data.session = buf.toString('latin1', off, off + kSessionLength);
        off += kSessionLength;
        this.stage = 'begin';
        continue;
      }
      if (this.stage === 'after-end') {
        if (off < buf.length) return this.fail('bytes after SnapshotEnd');
        break;
      }
      if (buf.length - off < 2) break;
      const len = buf.readUInt16LE(off);
      if (buf.length - off - 2 < len) break;
      const msg = buf.subarray(off + 2, off + 2 + len);
      off += 2 + len;
      const st = this.onMessage(msg);
      if (st === 'error') return st;
    }
    if (this.state === 'need-more' && this.stage === 'after-end') this.state = 'done';
    this.pending = off < buf.length ? Buffer.from(buf.subarray(off)) : Buffer.alloc(0);
    if (this.state === 'done' && this.pending.length) return this.fail('bytes after SnapshotEnd');
    return this.state;
  }

  onMessage(msg) {
    const type = msg.length ? msg[0] : -1;
    switch (this.stage) {
      case 'begin': {
        if (type !== kSnapBegin || msg.length !== SnapshotBeginSize) return this.fail('expected SnapshotBegin');
        this.trackCount = msg.readUInt32LE(4);
        if (this.trackCount > SnapshotParser.kMaxTracks) return this.fail('track_count too large');
        const asOf = u64(msg, 8);
        if (asOf < 0) return this.fail('as_of_seq out of range');
        this.data.asOfSeq = asOf;
        this.data.eventTsNs = msg.readBigUInt64LE(16);
        this.stage = this.trackCount === 0 ? 'end' : 'tracks';
        return this.state;
      }
      case 'tracks': {
        if (type !== kUpdate || msg.length !== TrackUpdateSize) return this.fail('expected TrackUpdate');
        const t = Buffer.from(msg);
        const n = this.data.tracks.length;
        if (n > 0 && this.data.tracks[n - 1].readUInt32LE(4) >= t.readUInt32LE(4)) {
          return this.fail('tracks not in increasing ICAO order');
        }
        this.data.tracks.push(t);
        if (this.data.tracks.length === this.trackCount) this.stage = 'end';
        return this.state;
      }
      case 'end': {
        if (type !== kSnapEnd || msg.length !== SnapshotEndSize) return this.fail('expected SnapshotEnd');
        if (u64(msg, 8) !== this.data.asOfSeq) return this.fail('SnapshotEnd as_of_seq differs');
        this.stage = 'after-end';
        return this.state;
      }
      default:
        return this.fail('internal');
    }
  }
}

// ---------------------------------------------------------------------------
// SubscriberCore
// ---------------------------------------------------------------------------

export const Source = Object.freeze({ kFeed: 0, kRetransmit: 1 });
export const Mode = Object.freeze({ kLive: 'live', kSnapshot: 'snapshot' });

export const defaultSubscriberConfig = Object.freeze({
  window: 1 << 14, // messages buffered ahead of `expected` (power of two, at most kMaxWindow)
  maxRequest: 1024, // messages per retransmit request
  retransmitTimeoutNs: 10e6, // re-request after this long without progress
  snapshotRetryNs: 500e6, // retry a failed snapshot after this long
});
export const kMaxWindow = 1 << 20;
const kRetiredSessions = 8;

function roundUpPow2(n) {
  if (n > kMaxWindow) n = kMaxWindow;
  let p = 2;
  while (p < n) p *= 2;
  return p;
}

function newStats() {
  return {
    feed_packets: 0,
    retransmit_packets: 0,
    heartbeats: 0,
    malformed: 0,
    applied: 0,
    duplicates: 0,
    buffered: 0,
    window_overflow: 0,
    oversize: 0,
    gaps: 0,
    requests: 0,
    request_retries: 0,
    truncated_replies: 0,
    naks: 0,
    snapshot_requests: 0,
    snapshots_loaded: 0,
    sessions_confirmed: 0,
    snapshot_failures: 0,
    session_changes: 0,
    stale_packets: 0,
    unknown_messages: 0,
  };
}

// handler: { onMessage(seq, msg), onReset(), onSnapshotLoaded() }, all optional.
// io: { sendRequest(session, seq, count) }.
export class SubscriberCore {
  constructor(cfg = {}, io = null, handler = null) {
    this.cfg = { ...defaultSubscriberConfig, ...cfg };
    this.cfg.window = roundUpPow2(this.cfg.window);
    if (!(this.cfg.maxRequest >= 1)) this.cfg.maxRequest = 1;
    this.cfg.maxRequest = Math.min(this.cfg.maxRequest, 0xffff);
    // Never ask for more than the window can buffer (C++ SubscriberCore:
    // reordered reply packets past the window are thrown away).
    this.cfg.maxRequest = Math.min(this.cfg.maxRequest, this.cfg.window);
    this.io = io;
    this.handler = handler;
    this.book = new FeedBook();
    this.mask = this.cfg.window - 1;
    this.pendSeq = new Float64Array(this.cfg.window); // 0 = empty
    this.pendMsg = new Array(this.cfg.window).fill(null);

    this.session = '';
    this.hasSession = false;
    this.retired = []; // most recent kRetiredSessions sessions we left

    this.mode = Mode.kLive;
    this.expected = 1;
    this.knownNext = 1;
    this.eos = false;
    this.eosSeq = 0;

    this.reqActive = false;
    this.reqFrom = 0;
    this.reqTo = 0;
    this.reqSentNs = 0;
    this.gapOpen = false;

    this.snapDue = false;
    this.snapLastNs = -Infinity;
    this.snapAnchor = 0;
    this.snapReqSession = '';
    this.confirming = false; // live, but a session change awaits the server's word

    this.stats = newStats();
  }

  // End of session seen, and every message before it applied.
  get ended() {
    return this.eos && this.mode === Mode.kLive && this.expected >= this.eosSeq;
  }

  isRetired(s) {
    return this.retired.includes(s);
  }

  retire(s) {
    if (this.isRetired(s)) return;
    this.retired.push(s);
    if (this.retired.length > kRetiredSessions) this.retired.shift();
  }

  unretire(s) {
    this.retired = this.retired.filter((r) => r !== s);
  }

  newSession(s) {
    if (this.hasSession) {
      this.retire(this.session);
      this.stats.session_changes++;
    }
    this.unretire(s);
    this.session = s;
    this.hasSession = true;
    this.book.clear();
    this.pendSeq.fill(0);
    this.pendMsg.fill(null);
    this.mode = Mode.kLive;
    this.expected = 1;
    this.knownNext = 1;
    this.eos = false;
    this.eosSeq = 0;
    this.reqActive = false;
    this.gapOpen = false;
    this.snapDue = false;
    this.snapAnchor = 0;
    this.confirming = false;
    this.handler?.onReset?.();
  }

  enterSnapshot() {
    this.mode = Mode.kSnapshot;
    this.reqActive = false;
    this.gapOpen = false;
    this.snapDue = true;
    this.snapAnchor = 0;
  }

  onDatagram(buf, src, nowNs) {
    const p = parsePacket(buf);
    if (!p) {
      this.stats.malformed++;
      return;
    }
    if (src === Source.kFeed) this.stats.feed_packets++;
    else this.stats.retransmit_packets++;
    if (
      p.seq === 0 ||
      (this.hasSession && p.session === this.session && p.seq > this.expected && p.seq - this.expected > kMaxSeqJump)
    ) {
      this.stats.malformed++;
      return;
    }

    if (!this.hasSession || p.session !== this.session) {
      // A delayed packet from a session we left, or a reply about another session.
      if (this.isRetired(p.session) || src === Source.kRetransmit) {
        this.stats.stale_packets++;
        return;
      }
      // The publisher (re)started. Joining the first session at seq 1 needs
      // nothing else. A *change* of session at seq 1 is confirmed with the
      // snapshot server in the background, staying live (C++ SubscriberCore:
      // a late packet of the old session can arrive after the new session's
      // first packets; unconfirmed, the subscriber would stay on the dead one).
      const change = this.hasSession;
      this.newSession(p.session);
      if (p.seq !== 1) {
        this.enterSnapshot();
      } else if (change) {
        this.confirming = true;
        this.snapDue = true;
      }
    }

    switch (p.kind) {
      case PacketKind.kEndOfSession:
        if (src === Source.kFeed) {
          this.eos = true;
          this.eosSeq = p.seq;
          this.knownNext = Math.max(this.knownNext, p.seq);
        }
        break;
      case PacketKind.kHeartbeat:
        if (src === Source.kRetransmit) {
          // NAK: p.seq is the oldest message the server still holds.
          if (this.mode === Mode.kLive && p.seq > this.expected) {
            this.stats.naks++;
            this.enterSnapshot();
          }
          return;
        }
        this.stats.heartbeats++;
        this.knownNext = Math.max(this.knownNext, p.seq);
        break;
      case PacketKind.kData:
        this.knownNext = Math.max(this.knownNext, p.seq + p.count);
        for (let i = 0; i < p.blocks.length; i++) {
          const [off, len] = p.blocks[i];
          this.accept(p.seq + i, buf.subarray(off, off + len));
        }
        // A reply packet with room for another message is the last of its
        // answer; if it answered the current request and brought us exactly
        // to its end without completing it, the server truncated the request:
        // ask for the rest now, not after the timeout (C++ SubscriberCore).
        if (
          src === Source.kRetransmit &&
          this.reqActive &&
          this.expected < this.reqTo &&
          p.seq >= this.reqFrom &&
          this.expected === p.seq + p.count &&
          buf.length + 2 + kMaxMessageBytes <= kMaxUdpPayload
        ) {
          this.reqTo = this.expected;
          this.stats.truncated_replies++;
        }
        break;
    }
    this.maybeRequest(nowNs);
  }

  accept(seq, msg) {
    if (seq < this.expected) {
      this.stats.duplicates++;
      return;
    }
    if (seq === this.expected) {
      this.apply(seq, msg);
      this.expected++;
      this.drainPending();
      return;
    }
    // Ahead: buffer inside the window. In snapshot mode the window is anchored
    // at the first message buffered (`expected` may be far behind).
    let inWindow = seq - this.expected < this.cfg.window;
    if (!inWindow && this.mode === Mode.kSnapshot) {
      if (this.snapAnchor === 0) this.snapAnchor = seq;
      inWindow = seq >= this.snapAnchor && seq - this.snapAnchor < this.cfg.window;
    }
    if (!inWindow) {
      this.stats.window_overflow++;
      return;
    }
    if (msg.length > kMaxMessageBytes) {
      this.stats.oversize++;
      return;
    }
    const slot = seq & this.mask; // seq < 2^53; & works on the low 32 bits, and mask < 2^21
    if (this.pendSeq[slot] === seq) {
      this.stats.duplicates++;
      return;
    }
    // Every lookup checks the exact seq, so overwriting a congruent entry only
    // costs a later re-request.
    this.pendSeq[slot] = seq;
    this.pendMsg[slot] = Buffer.from(msg); // the datagram buffer is not ours to keep
    this.stats.buffered++;
  }

  apply(seq, msg) {
    const r = this.book.apply(msg);
    if (r === ApplyResult.kUnknownType || r === ApplyResult.kMalformed) this.stats.unknown_messages++;
    this.stats.applied++;
    this.handler?.onMessage?.(seq, msg, r);
  }

  drainPending() {
    for (;;) {
      const slot = this.expected & this.mask;
      if (this.pendSeq[slot] !== this.expected) return;
      const msg = this.pendMsg[slot];
      this.pendSeq[slot] = 0;
      this.pendMsg[slot] = null;
      this.apply(this.expected, msg);
      this.expected++;
    }
  }

  maybeRequest(nowNs) {
    if (this.mode !== Mode.kLive) return;
    if (this.knownNext <= this.expected) {
      this.reqActive = false;
      this.gapOpen = false;
      return;
    }
    if (!this.gapOpen) {
      this.gapOpen = true;
      this.stats.gaps++;
    }
    if (!this.io) return;
    let retry = false;
    if (this.reqActive) {
      const complete = this.expected >= this.reqTo;
      if (!complete && nowNs - this.reqSentNs < this.cfg.retransmitTimeoutNs) return;
      retry = !complete;
    }
    let hi = Math.min(this.knownNext, this.expected + this.cfg.maxRequest);
    for (let s = this.expected + 1; s < hi; s++) {
      if (this.pendSeq[s & this.mask] === s) {
        hi = s;
        break;
      }
    }
    this.io.sendRequest(this.session, this.expected, hi - this.expected);
    this.reqActive = true;
    this.reqFrom = this.expected;
    this.reqTo = hi;
    this.reqSentNs = nowNs;
    this.stats.requests++;
    if (retry) this.stats.request_retries++;
  }

  poll(nowNs) {
    this.maybeRequest(nowNs);
  }

  // True when the caller should fetch a snapshot now (then call onSnapshot or
  // onSnapshotFailed). Rate-limited by snapshotRetryNs.
  takeSnapshotRequest(nowNs) {
    if (this.mode !== Mode.kSnapshot && !this.confirming) return false;
    if (!this.snapDue && nowNs - this.snapLastNs < this.cfg.snapshotRetryNs) return false;
    this.snapDue = false;
    this.snapLastNs = nowNs;
    this.snapReqSession = this.session;
    this.stats.snapshot_requests++;
    return true;
  }

  onSnapshotFailed(nowNs) {
    this.stats.snapshot_failures++;
    this.snapLastNs = nowNs;
  }

  // snap: { session, asOfSeq, tracks: [Buffer(64)] } (SnapshotParser.data)
  onSnapshot(snap, nowNs) {
    if (this.mode !== Mode.kSnapshot) {
      if (!this.confirming) return;
      if (this.hasSession && snap.session === this.session) {
        this.confirming = false; // the server agrees: keep our state
        this.stats.sessions_confirmed++;
        return;
      }
      // The server is in another session: adopt it, as below.
    }
    if (!this.hasSession || snap.session !== this.session) {
      // Only an answer to a request made in the current session says which
      // session is live (the publisher's): ours came from a stray or late
      // packet. One asked for before we changed session is stale whatever
      // session it carries (C++ SubscriberCore: a server that died since may
      // have answered it).
      if (this.snapReqSession !== this.session) {
        this.onSnapshotFailed(nowNs);
        return;
      }
      this.newSession(snap.session);
      this.mode = Mode.kSnapshot;
    }
    // Adopt unless we already hold a later state (TLA+: e == Max(expected, S + 1)).
    if (snap.asOfSeq + 1 >= this.expected) {
      this.book.clear();
      for (const t of snap.tracks) this.book.put(t);
      this.expected = snap.asOfSeq + 1;
      this.handler?.onSnapshotLoaded?.();
    }
    this.knownNext = Math.max(this.knownNext, snap.asOfSeq + 1);
    this.confirming = false;
    this.mode = Mode.kLive;
    this.snapAnchor = 0;
    this.stats.snapshots_loaded++;
    this.drainPending();
    this.maybeRequest(nowNs);
  }
}
