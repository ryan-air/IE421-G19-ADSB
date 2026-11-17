#!/usr/bin/env node
// adsb-ws-client: a browser stand-in. Connects to the gateway's WebSocket,
// keeps the track table exactly as the map does (gateway/public/
// gateway_protocol.mjs), and reports what it received, the end-to-end
// latency (publish -> WebSocket message received) and the table's digest.
//
//   adsb-ws-client [--url ws://127.0.0.1:8080/gateway/ws] [--until-end] [--duration S]
//                  [--clock-helper PATH|none] [--slow-ms N] [--window N] [--quiet]
//
// --slow-ms N stalls the event loop for N ms after every frame, to act as a
// slow browser. --window N (default 8) asks the gateway for flow control: at
// most N frames unacknowledged, acknowledged every N/2 once processed, so a slow
// client gets conflated frames and stays current. --window 0 turns it off
// (then a slow client's backlog builds up in the kernel socket buffers).
// The last line is the digest of the table; once the session has ended it
// equals the publisher's "book digest".

import { existsSync } from 'node:fs';
import { join } from 'node:path';

import { FrameFlags, FrameKind, TrackTable, decodeFrame } from '../public/gateway_protocol.mjs';
import { ClockSync, LatencyHistogram } from '../src/clock.mjs';
import { bookDigest } from '../src/feed.mjs';
import { repoRoot } from '../src/gateway.mjs';

const args = process.argv.slice(2);
const opt = { url: 'ws://127.0.0.1:8080/gateway/ws', slowMs: 0, window: 8 };
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  const val = () => args[++i];
  if (a === '--url') opt.url = val();
  else if (a === '--until-end') opt.untilEnd = true;
  else if (a === '--duration') opt.duration = Number(val());
  else if (a === '--clock-helper') opt.clockHelper = val();
  else if (a === '--slow-ms') opt.slowMs = Number(val());
  else if (a === '--window') opt.window = Number(val());
  else if (a === '--quiet') opt.quiet = true;
  else {
    console.error(`unknown option ${a}`);
    process.exit(2);
  }
}

let helper = opt.clockHelper;
if (helper === undefined) {
  helper = ['release', 'debug'].map((p) => join(repoRoot, 'core', 'build', p, 'tools', 'adsb-clock')).find((p) => existsSync(p)) ?? null;
} else if (helper === 'none') helper = null;
const clock = new ClockSync(helper);
await clock.start();

const table = new TrackTable();
const lat = new LatencyHistogram();
const counts = { frames: 0, snapshots: 0, messages: 0, bytes: 0, status: 0 };
let ended = false;
let status = null;

// The gateway may still be starting: retry the connection for up to 10 s.
async function connect() {
  const deadline = Date.now() + 10_000;
  for (;;) {
    const ws = new WebSocket(opt.url);
    ws.binaryType = 'arraybuffer';
    const ok = await new Promise((resolve) => {
      ws.onopen = () => resolve(true);
      ws.onerror = () => resolve(false);
    });
    if (ok) return ws;
    if (Date.now() > deadline) {
      console.error(`adsb-ws-client: cannot connect to ${opt.url}`);
      process.exit(1);
    }
    await new Promise((r) => setTimeout(r, 100));
  }
}
const ws = await connect();
if (opt.window > 0) ws.send(JSON.stringify({ type: 'hello', window: opt.window }));

const busy = (ms) => {
  const t = Date.now() + ms;
  while (Date.now() < t);
};

const ack = new Uint8Array(5);
ack[0] = 0x41; // 'A'
const ackView = new DataView(ack.buffer);
const ackEvery = Math.max(1, Math.ceil(opt.window / 2));
let acked = 0;

const done = new Promise((resolve, reject) => {
  ws.onerror = (e) => reject(new Error(`websocket error: ${e.message ?? 'connection failed'}`));
  ws.onclose = () => resolve('closed');
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      counts.status++;
      status = JSON.parse(ev.data);
      return;
    }
    const now = clock.now();
    const frame = decodeFrame(ev.data);
    counts.frames++;
    counts.bytes += ev.data.byteLength;
    counts.messages += frame.messages.length;
    if (frame.kind === FrameKind.kSnapshot) counts.snapshots++;
    else {
      for (const m of frame.messages) {
        if (m[0] !== 0x55) continue;
        const pub = Number(new DataView(m.buffer, m.byteOffset, m.byteLength).getBigUint64(56, true));
        if (pub !== 0) lat.record(now - pub);
      }
    }
    table.apply(frame);
    ended = (frame.flags & FrameFlags.kEnded) !== 0 && (frame.flags & FrameFlags.kStale) === 0;
    if (opt.untilEnd && ended) resolve('ended');
    if (opt.slowMs > 0) busy(opt.slowMs);
    // Acknowledge every ceil(window / 2) frames: enough to never run the
    // gateway out of credit while keeping up, at a fraction of the messages.
    if (opt.window > 0 && counts.frames - acked >= ackEvery && ws.readyState === WebSocket.OPEN) {
      acked = counts.frames;
      ackView.setUint32(1, acked, true); // cumulative: frames processed so far
      ws.send(ack);
    }
  };
});

let timer = null;
if (opt.duration > 0) timer = setTimeout(() => ws.close(), opt.duration * 1000);

let why;
try {
  why = await done;
} catch (e) {
  console.error(`adsb-ws-client: ${e.message}`);
  process.exit(1);
}
clearTimeout(timer);
if (ws.readyState === WebSocket.OPEN) ws.close();
clock.stop();

const h = lat.summary();
const ck = clock.describe();
console.log(
  `frames ${counts.frames} (snapshots ${counts.snapshots}) messages ${counts.messages} bytes ${counts.bytes} status ${counts.status}`,
);
console.log(
  `publish -> client: ${h.count ? `p50 ${h.p50_us} p90 ${h.p90_us} p99 ${h.p99_us} p99.9 ${h.p999_us} max ${h.max_us} µs (n=${h.count})` : 'n=0'}` +
    (ck.calibrated ? ` [clock ±${ck.uncertainty_ns} ns]` : ' [uncalibrated]'),
);
const toBuf = new Map([...table.tracks].map(([k, v]) => [k, Buffer.from(v)]));
console.log(`as_of_seq ${table.asOfSeq} tracks ${table.tracks.size} ended ${ended} session ${status?.session ?? '?'}`);
console.log(`book digest ${bookDigest(toBuf)}`);
process.exit(opt.untilEnd && !ended ? 3 : 0);
