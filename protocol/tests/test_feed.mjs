// Checks the generated JavaScript feed module against the golden packet.
// Run from the repository root:  node protocol/tests/test_feed.mjs

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import * as fm from '../generated/feed_messages.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const raw = readFileSync(join(root, 'golden', 'packet_v0.bin'));
const expected = JSON.parse(readFileSync(join(root, 'golden', 'packet_v0.json'), 'utf8'));

// JSON has no BigInt: compare 64-bit fields as strings.
const norm = (obj) =>
  Object.fromEntries(Object.entries(obj).map(([k, v]) => [k, typeof v === 'bigint' ? v.toString() : v]));
// The golden JSON stores 64-bit fields as strings (JSON.parse would round them).
const normExpected = (obj) => obj;

let passed = 0;
const test = (name, fn) => {
  fn();
  passed++;
  console.log(`ok - ${name}`);
};

test('golden packet decodes to the schema example values', () => {
  const { header, messages } = fm.decodePacket(new Uint8Array(raw));
  assert.equal(header.session, expected.header.session);
  assert.equal(header.seq, BigInt(expected.header.seq));
  assert.equal(header.count, expected.header.count);
  assert.equal(messages.length, expected.messages.length);
  messages.forEach((m, i) => {
    assert.equal(m.seq, header.seq + BigInt(i));
    assert.equal(m.name, expected.messages[i].name);
    assert.deepEqual(norm(m.msg), normExpected(expected.messages[i].fields));
  });
});

test('re-encoding reproduces the golden bytes', () => {
  const { header, messages } = fm.decodePacket(new Uint8Array(raw));
  const out = new DataView(new ArrayBuffer(raw.length));
  fm.encodePacketHeader(header, out, 0);
  let off = fm.PacketHeaderSize;
  for (const m of messages) {
    const size = fm[`${m.name}Size`];
    out.setUint16(off, size, true);
    fm[`encode${m.name}`](m.msg, out, off + 2);
    off += 2 + size;
  }
  assert.deepEqual(new Uint8Array(out.buffer), new Uint8Array(raw));
});

test('truncated packets throw instead of reading past the end', () => {
  for (let n = 0; n < raw.length; n++) {
    assert.throws(() => fm.decodePacket(new Uint8Array(raw.subarray(0, n))), RangeError, `length ${n}`);
  }
});

test('rejects what the C++ parser rejects', () => {
  const withTail = new Uint8Array(raw.length + 3);
  withTail.set(raw);
  assert.throws(() => fm.decodePacket(withTail), RangeError);
  const hb = new Uint8Array(raw.subarray(0, 20));
  for (const count of [fm.kHeartbeatCount, fm.kEndOfSessionCount]) {
    new DataView(hb.buffer).setUint16(18, count, true);
    fm.decodePacket(hb);  // bare heartbeat / end-of-session: fine
    const withBody = new Uint8Array(24);
    withBody.set(hb);
    assert.throws(() => fm.decodePacket(withBody), RangeError);
  }
});

test('null constants and callsign padding', () => {
  assert.equal(fm.TrackUpdateNull.lat_e7, -(2 ** 31));
  const view = new DataView(new ArrayBuffer(64));
  const { messages } = fm.decodePacket(new Uint8Array(raw));
  fm.encodeTrackUpdate({ ...messages[0].msg, callsign: 'AB' }, view, 0);
  assert.equal(fm.decodeTrackUpdate(view, 0).callsign, 'AB      ');
});

test('sizes and bit constants', () => {
  assert.equal(fm.TrackUpdateSize, 64);
  assert.equal(fm.PacketHeaderSize, 20);
  assert.equal(fm.UpdateMask.kNewTrack, 1 << 15);
  assert.equal(fm.kMaxUdpPayload, 1472);
});

console.log(`${passed} tests passed`);
