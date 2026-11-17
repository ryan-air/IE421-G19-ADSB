// Gateway → browser WebSocket format (gateway/README.md §Protocol). Shared by
// the gateway (Node) and the browser, so it uses only DataView / Uint8Array.
//
// Every binary WebSocket message is one frame:
//
//   offset  field     type   meaning
//   0       kind      u8     'S' snapshot: replace the whole table
//                            'D' delta: apply on top of the table
//   1       version   u8     0
//   2       flags     u16    bit 0 STALE: the gateway is recovering from feed loss,
//                                  so the table may be behind the publisher
//                            bit 1 ENDED: the publisher's session has ended
//   4       count     u32    number of messages that follow
//   8       as_of_seq u64    after this frame the table equals the publisher's
//                            book after feed message `as_of_seq`
//   16      count x { u16 length, feed message } (TrackUpdate 'U' or TrackDelete 'D',
//                            exactly as on the feed: protocol/feed_spec.md §3)
//
// All integers little-endian, like the feed. Deltas are conflated per client:
// a track that changed several times since the last frame is sent once, with
// its latest state, so a slow browser receives less, never stale data.
// Text messages are JSON status objects ({"type":"status", ...}).

export const kFrameHeaderSize = 16;
export const kFrameVersion = 0;
export const FrameKind = Object.freeze({ kSnapshot: 0x53, kDelta: 0x44 });
export const FrameFlags = Object.freeze({ kStale: 1, kEnded: 2 });

const kUpdate = 0x55;
const kDelete = 0x44;
const kTrackUpdateSize = 64;
const kTrackDeleteSize = 16;

export function encodeFrameHeader(kind, flags, count, asOfSeq) {
  const b = new Uint8Array(kFrameHeaderSize);
  const v = new DataView(b.buffer);
  v.setUint8(0, kind);
  v.setUint8(1, kFrameVersion);
  v.setUint16(2, flags, true);
  v.setUint32(4, count, true);
  v.setBigUint64(8, BigInt(asOfSeq), true);
  return b;
}

// Returns { kind, flags, asOfSeq, messages: [Uint8Array view] } or throws RangeError.
export function decodeFrame(data) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.byteLength < kFrameHeaderSize) throw new RangeError('frame shorter than its header');
  const kind = v.getUint8(0);
  if (kind !== FrameKind.kSnapshot && kind !== FrameKind.kDelta) throw new RangeError('unknown frame kind');
  if (v.getUint8(1) !== kFrameVersion) throw new RangeError('unsupported frame version');
  const flags = v.getUint16(2, true);
  const count = v.getUint32(4, true);
  const asOfSeq = Number(v.getBigUint64(8, true));
  const messages = [];
  let off = kFrameHeaderSize;
  for (let i = 0; i < count; i++) {
    if (off + 2 > bytes.byteLength) throw new RangeError('truncated message length');
    const len = v.getUint16(off, true);
    off += 2;
    if (off + len > bytes.byteLength) throw new RangeError('truncated message');
    messages.push(bytes.subarray(off, off + len));
    off += len;
  }
  if (off !== bytes.byteLength) throw new RangeError('trailing bytes after the last message');
  return { kind, flags, asOfSeq, messages };
}

// The browser's copy of the track table: icao -> Uint8Array(64) TrackUpdate.
export class TrackTable {
  constructor() {
    this.tracks = new Map();
    this.asOfSeq = 0;
    this.flags = 0;
    this.frames = 0;
  }

  // Applies one decoded frame. Returns the ICAO addresses it touched
  // (null for a snapshot: everything may have changed).
  apply(frame) {
    this.frames++;
    this.flags = frame.flags;
    this.asOfSeq = frame.asOfSeq;
    const snapshot = frame.kind === FrameKind.kSnapshot;
    if (snapshot) this.tracks.clear();
    const touched = snapshot ? null : [];
    for (const m of frame.messages) {
      if (m.length === 0) continue;
      const v = new DataView(m.buffer, m.byteOffset, m.byteLength);
      if (m[0] === kUpdate && m.length >= kTrackUpdateSize) {
        const icao = v.getUint32(4, true);
        this.tracks.set(icao, m.slice(0, kTrackUpdateSize));
        touched?.push(icao);
      } else if (m[0] === kDelete && m.length >= kTrackDeleteSize) {
        const icao = v.getUint32(4, true);
        this.tracks.delete(icao);
        touched?.push(icao);
      }
      // Unknown types are skipped (forward compatibility).
    }
    return touched;
  }
}

const INT32_NULL = -2147483648;
const U16_NULL = 0xffff;
const I16_NULL = -32768;

// One TrackUpdate in the JSON shape the Fall 2025 map expects from
// /api/aircraft (ads/server.js): null for unknown fields.
export function toAircraft(u, seenSec = null) {
  const v = new DataView(u.buffer, u.byteOffset, u.byteLength);
  const icao = v.getUint32(4, true);
  const lat = v.getInt32(24, true);
  const lon = v.getInt32(28, true);
  const alt = v.getInt32(32, true);
  const gs = v.getUint16(36, true);
  const trk = v.getUint16(38, true);
  const vr = v.getInt16(40, true);
  const sq = v.getUint16(42, true);
  let cs = '';
  for (let i = 44; i < 52; i++) cs += String.fromCharCode(u[i]);
  cs = cs.trim();
  return {
    hex: icao.toString(16).toUpperCase().padStart(6, '0'),
    flight: cs || null,
    lat: lat === INT32_NULL ? null : lat / 1e7,
    lon: lon === INT32_NULL ? null : lon / 1e7,
    altitude: alt === INT32_NULL ? null : alt,
    speed: gs === U16_NULL ? null : Math.round(gs / 10),
    track: trk === U16_NULL ? null : Math.round(trk / 100),
    vertical_rate: vr === I16_NULL ? null : vr,
    squawk: sq === U16_NULL ? null : sq.toString(16).padStart(4, '0'),
    on_ground: (u[54] & 2) !== 0,
    seen: seenSec,
  };
}
