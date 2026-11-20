// The gateway end to end, in one process on real sockets: a JS publisher
// (UDP feed, retransmit and TCP snapshot servers) -> Gateway -> WebSocket
// clients. Every client's table must end byte-identical to the publisher's
// book, through loss, restarts, late joins and slow clients.

import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import http from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { FrameFlags, FrameKind, TrackTable, decodeFrame } from '../public/gateway_protocol.mjs';
import { bookDigest } from '../src/feed.mjs';
import { Gateway } from '../src/gateway.mjs';
import { NetPublisher, RawClient, maskedFrame, sleep, waitFor } from './net_fixture.mjs';
import { Rng, S1, S2, trackDelete, trackUpdate } from './helpers.mjs';

const tableDigest = (tracks) => bookDigest(new Map([...tracks].map(([k, v]) => [k, Buffer.from(v)])));

function randomTraffic(n, { seed = 1, icaos = 50, deletes = 0.1, ts0 = 0 } = {}) {
  const rng = new Rng(seed);
  const out = [];
  for (let i = 0; i < n; i++) {
    const icao = 0x400000 + rng.below(icaos);
    out.push(
      rng.chance(deletes)
        ? trackDelete(icao, ts0 + i)
        : trackUpdate({
            icao,
            alt: rng.below(40000),
            lat: 40 + rng.next(),
            lon: -88 - rng.next(),
            mask: 1 | 2,
            eventTs: ts0 + i,
            callsign: `T${icao.toString(16)}`,
          }),
    );
  }
  return out;
}

async function setup({ pubOpts = {}, gwOpts = {}, loss = {}, core = {} } = {}) {
  const pub = await new NetPublisher(S1, pubOpts).open();
  // base/map is served; base/secret.json and base/map/.hidden.json must never be.
  const base = mkdtempSync(join(tmpdir(), 'gw-map-'));
  const mapDir = join(base, 'map');
  mkdirSync(mapDir);
  writeFileSync(join(base, 'secret.json'), '{"secret":1}');
  writeFileSync(join(mapDir, '.hidden.json'), '{"secret":2}');
  writeFileSync(join(mapDir, 'index.html'), '<html><body><script src="app.js"></script>\n</body></html>');
  writeFileSync(join(mapDir, 'app.js'), 'console.log(1)');
  writeFileSync(join(mapDir, '.env'), 'DATABASE_URL=secret');
  writeFileSync(join(mapDir, 'aircraft-types-by-hex.json'), JSON.stringify({ '400001': 6 }));
  const gw = new Gateway({
    listen: { host: '127.0.0.1', port: 0 },
    mapDir,
    clockHelper: null,
    statusMs: 50,
    ...gwOpts,
    feedOptions: {
      feed: { host: '127.0.0.1', port: 0 },
      retransmit: pub.retransmit,
      snapshot: pub.snapshot,
      loss,
      core,
      pollMs: 1,
    },
  });
  await gw.start();
  pub.dest = { host: '127.0.0.1', port: gw.feed.feedPort() };
  const port = gw.address().port;
  const cleanup = async () => {
    await gw.stop().catch(() => {});
    pub.close();
    rmSync(base, { recursive: true, force: true });
  };
  return { pub, gw, port, mapDir, cleanup };
}

// A browser stand-in on Node's WebSocket client.
async function browser(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/gateway/ws`);
  ws.binaryType = 'arraybuffer';
  const b = { ws, table: new TrackTable(), frames: [], status: [] };
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') b.status.push(JSON.parse(e.data));
    else {
      const f = decodeFrame(e.data);
      b.frames.push(f);
      b.table.apply(f);
    }
  };
  await new Promise((r, j) => {
    ws.onopen = r;
    ws.onerror = j;
  });
  b.digest = () => tableDigest(b.table.tracks);
  return b;
}

const settled = (gw, pub, b) =>
  gw.core.expected === pub.pub.next && b.table.asOfSeq === pub.pub.next - 1 && !(b.table.flags & FrameFlags.kStale);

test('clients get a snapshot on connect, then deltas; tables equal the publisher book', async () => {
  const { pub, gw, port, cleanup } = await setup();
  try {
    pub.publish(randomTraffic(300, { seed: 1 }));
    await waitFor(() => gw.core.expected === 301, 3000, 'gateway applies');
    const a = await browser(port);
    await waitFor(() => a.frames.length >= 1, 2000, 'first frame');
    assert.equal(a.frames[0].kind, FrameKind.kSnapshot);
    assert.equal(a.digest(), pub.pub.book.hexDigest());
    const b = await browser(port);
    pub.publish(randomTraffic(500, { seed: 2, ts0: 1000 }));
    pub.heartbeat();
    await waitFor(() => settled(gw, pub, a) && settled(gw, pub, b), 3000, 'clients settle');
    for (const c of [a, b]) {
      assert.equal(c.digest(), pub.pub.book.hexDigest());
      assert.ok(c.frames.slice(1).every((f) => f.kind === FrameKind.kDelta));
      assert.ok(c.status.length >= 1 && c.status[0].type === 'status' && c.status[0].session === S1);
    }
    assert.equal(gw.digest().digest, pub.pub.book.hexDigest());
    a.ws.close();
    b.ws.close();
  } finally {
    await cleanup();
  }
});

test('feed loss is recovered by retransmit and clients converge', async () => {
  const { pub, gw, port, cleanup } = await setup({ loss: { dropRate: 0.2, seed: 9 } });
  try {
    const a = await browser(port);
    for (let i = 0; i < 20; i++) {
      pub.publish(randomTraffic(100, { seed: 10 + i, ts0: i * 100 }));
      await sleep(2);
    }
    pub.heartbeat();
    await waitFor(() => settled(gw, pub, a), 5000, 'recovery');
    assert.ok(gw.core.stats.requests > 0);
    assert.ok(gw.feed.loss.dropped > 0);
    assert.equal(a.digest(), pub.pub.book.hexDigest());
    a.ws.close();
  } finally {
    await cleanup();
  }
});

test('a gap older than the ring is recovered by a TCP snapshot', async () => {
  const { pub, gw, port, cleanup } = await setup({ pubOpts: { ring: 64 } });
  try {
    const a = await browser(port);
    pub.publish(randomTraffic(20, { seed: 3 }));
    await waitFor(() => gw.core.expected === 21, 2000, 'first messages');
    const dest = pub.dest;
    pub.dest = null; // the gateway hears nothing for a while
    pub.publish(randomTraffic(500, { seed: 4, ts0: 100 }));
    pub.dest = dest;
    pub.publish(randomTraffic(10, { seed: 5, ts0: 1000 }));
    pub.heartbeat();
    await waitFor(() => settled(gw, pub, a), 5000, 'snapshot recovery');
    assert.ok(gw.core.stats.naks >= 1);
    assert.ok(gw.core.stats.snapshots_loaded >= 1);
    assert.ok(pub.snapshots >= 1);
    assert.equal(a.digest(), pub.pub.book.hexDigest());
    assert.ok(a.frames.some((f, i) => i > 0 && f.kind === FrameKind.kSnapshot), 'clients get a fresh snapshot frame');
    assert.ok(a.frames.some((f) => f.flags & FrameFlags.kStale), 'clients were told the data was stale');
    a.ws.close();
  } finally {
    await cleanup();
  }
});

test('a gateway that joins mid-session takes a snapshot', async () => {
  const pubOnly = await new NetPublisher(S1, { ring: 4096 }).open();
  // Published before the gateway exists (nobody listening).
  pubOnly.publish(randomTraffic(400, { seed: 6 }));
  pubOnly.close();
  const { pub, gw, port, cleanup } = await setup();
  try {
    pub.pub = pubOnly.pub; // same session and history, served by the new sockets
    pub.publish(randomTraffic(5, { seed: 7, ts0: 500 }));
    const a = await browser(port);
    await waitFor(() => settled(gw, pub, a), 5000, 'late join');
    assert.equal(gw.core.stats.snapshots_loaded, 1);
    assert.equal(a.digest(), pub.pub.book.hexDigest());
    a.ws.close();
  } finally {
    await cleanup();
  }
});

test('publisher restart: clients are reset to the new session', async () => {
  const { pub, gw, port, cleanup } = await setup();
  try {
    const a = await browser(port);
    pub.publish(randomTraffic(200, { seed: 8 }));
    await waitFor(() => settled(gw, pub, a), 3000, 'first session');
    pub.restart(S2);
    pub.publish(randomTraffic(50, { seed: 9, icaos: 5 }));
    pub.heartbeat();
    await waitFor(() => gw.core.session === S2 && settled(gw, pub, a), 3000, 'second session');
    assert.equal(a.digest(), pub.pub.book.hexDigest());
    assert.ok(a.table.tracks.size <= 5, 'nothing from the old session survives');
    await waitFor(() => a.status.some((s) => s.session === S2), 2000, 'status names the new session');
    a.ws.close();
  } finally {
    await cleanup();
  }
});

test('end of session is flagged to clients', async () => {
  const { pub, gw, port, cleanup } = await setup();
  try {
    const a = await browser(port);
    pub.publish(randomTraffic(30, { seed: 11 }));
    pub.heartbeat(true);
    await waitFor(() => a.table.flags & FrameFlags.kEnded, 3000, 'ENDED flag');
    assert.ok(gw.core.ended);
    assert.equal(a.digest(), pub.pub.book.hexDigest());
    a.ws.close();
  } finally {
    await cleanup();
  }
});

test('a slow client is conflated (bounded), never blocks others, and still converges', async () => {
  const { pub, gw, port, cleanup } = await setup({ gwOpts: { highWater: 16 * 1024 } });
  try {
    const fast = await browser(port);
    const slow = await new RawClient(port).connect();
    await waitFor(() => slow.frames.length >= 1, 2000, 'slow client snapshot');
    slow.pause();
    // Many updates to few aircraft: lots to conflate.
    // Publish until the kernel buffers are full and the gateway starts
    // skipping the slow client, then a good while longer.
    let published = 0;
    let extra = -1;
    for (let i = 0; i < 2000 && extra !== 0; i++) {
      pub.publish(randomTraffic(200, { seed: 100 + i, icaos: 20, deletes: 0.05, ts0: i * 200 }));
      published += 200;
      if (extra < 0 && gw.counters.skipped_flushes > 0) extra = 40;
      else if (extra > 0) extra--;
      await sleep(1);
    }
    pub.heartbeat();
    await waitFor(() => settled(gw, pub, fast), 5000, 'fast client keeps up');
    assert.equal(fast.digest(), pub.pub.book.hexDigest());
    const slowEntry = [...gw.clients].find((c) => c.ws.socket.remotePort === slow.socket.localPort);
    assert.ok(slowEntry, 'slow client still connected');
    assert.ok(slowEntry.dirty.size <= 20, 'pending state is bounded by the number of tracks');
    assert.ok(gw.counters.skipped_flushes > 0, 'the slow client was skipped');
    assert.ok(gw.counters.conflated > 0);
    slow.resume();
    await waitFor(
      () => slow.frames.length > 0 && slow.frames.at(-1).asOfSeq === pub.pub.next - 1,
      5000,
      'slow client catches up',
    );
    const table = new TrackTable();
    for (const f of slow.frames) table.apply(f);
    assert.equal(tableDigest(table.tracks), pub.pub.book.hexDigest());
    // It received far fewer messages than were published.
    const received = slow.frames.reduce((n, f) => n + f.messages.length, 0);
    assert.ok(received < published / 2, `received ${received} of ${published}`);
    fast.ws.close();
    slow.close();
  } finally {
    await cleanup();
  }
});

test('a client stuck over the high-water mark is disconnected', async () => {
  const { pub, gw, port, cleanup } = await setup({ gwOpts: { highWater: 4096, stuckMs: 100 } });
  try {
    const slow = await new RawClient(port).connect();
    slow.pause();
    const t0 = Date.now();
    // Big frames (many distinct aircraft) fill the kernel buffers quickly;
    // macOS grows loopback buffers, so keep going until the gateway drops it.
    let i = 0;
    while (Date.now() - t0 < 15000 && gw.counters.clients_dropped_stuck === 0) {
      pub.publish(randomTraffic(1000, { seed: ++i, icaos: 5000, deletes: 0 }), 20);
      await sleep(50); // ~20k messages/s: fast enough, without overrunning the gateway's UDP buffer
    }
    assert.equal(gw.counters.clients_dropped_stuck, 1);
    await waitFor(() => gw.clients.size === 0, 2000, 'client removed');
    slow.close();
  } finally {
    await cleanup();
  }
});

const hello = (window) => maskedFrame(1, Buffer.from(JSON.stringify({ type: 'hello', window })));
const ackFrame = (n) => {
  const b = Buffer.alloc(5);
  b[0] = 0x41;
  b.writeUInt32LE(n, 1);
  return maskedFrame(2, b);
};

test('flow control: at most `window` frames unacknowledged, the rest conflated', async () => {
  const { pub, gw, port, cleanup } = await setup();
  try {
    const c = await new RawClient(port).connect();
    c.send(hello(2));
    await waitFor(() => [...gw.clients][0]?.window === 2, 2000, 'hello');
    // Never acknowledging: the gateway may send 2 frames and no more.
    for (let i = 0; i < 20; i++) {
      pub.publish(randomTraffic(20, { seed: 500 + i, icaos: 10, ts0: i * 20 }));
      await sleep(3);
    }
    await sleep(50);
    assert.equal(c.frames.length, 2, 'snapshot + one delta, then held');
    assert.ok(gw.counters.flow_holds > 0);
    const entry = [...gw.clients][0];
    assert.ok(entry.dirty.size > 0 && entry.dirty.size <= 10);
    // Acknowledge both: the held changes arrive as one conflated frame that
    // brings the client exactly up to date.
    pub.heartbeat();
    c.send(ackFrame(2));
    await waitFor(() => c.frames.length === 3, 2000, 'conflated frame');
    const table = new TrackTable();
    for (const f of c.frames) table.apply(f);
    assert.equal(c.frames[2].asOfSeq, pub.pub.next - 1);
    assert.equal(tableDigest(table.tracks), pub.pub.book.hexDigest());
    assert.ok(c.frames[2].messages.length <= 10);
    c.close();
  } finally {
    await cleanup();
  }
});

test('flow control: bad hello, bad ack, and an unacknowledging client', async () => {
  const { pub, gw, port, cleanup } = await setup({ gwOpts: { stuckMs: 100 } });
  try {
    const closeCode = async (bytes) => {
      const c = await new RawClient(port).connect();
      c.send(bytes);
      await waitFor(() => c.raw.some((f) => f.opcode === 8), 2000, 'close');
      const code = c.raw.find((f) => f.opcode === 8).payload.readUInt16BE(0);
      c.close();
      return code;
    };
    assert.equal(await closeCode(hello(0)), 1008);
    assert.equal(await closeCode(hello(65)), 1008);
    assert.equal(await closeCode(maskedFrame(1, Buffer.from('not json'))), 1007);
    assert.equal(await closeCode(ackFrame(99)), 1008, 'ack for frames never sent');
    assert.equal(await closeCode(maskedFrame(2, Buffer.from([0x42, 0, 0, 0, 0]))), 1003);
    // A client that asked for flow control and then never acknowledges is
    // dropped after stuckMs.
    const c = await new RawClient(port).connect();
    c.send(hello(1));
    await waitFor(() => c.frames.length === 1, 2000, 'snapshot');
    const t0 = Date.now();
    while (Date.now() - t0 < 2000 && gw.counters.clients_dropped_stuck === 0) {
      pub.publish(randomTraffic(5, { seed: Date.now() & 0xffff }));
      await sleep(20);
    }
    assert.equal(gw.counters.clients_dropped_stuck, 1);
    c.close();
  } finally {
    await cleanup();
  }
});

test('client limit and unknown WebSocket paths are refused', async () => {
  const { gw, port, cleanup } = await setup({ gwOpts: { maxClients: 1 } });
  try {
    const a = await new RawClient(port).connect();
    assert.ok(a.upgraded);
    const b = await new RawClient(port).connect();
    assert.match(b.response, /^HTTP\/1.1 503/);
    assert.equal(gw.counters.clients_rejected, 1);
    const c = await new RawClient(port, '/other').connect();
    assert.match(c.response, /^HTTP\/1.1 404/);
    a.close();
    b.close();
    c.close();
  } finally {
    await cleanup();
  }
});

function get(port, path, method = 'GET') {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path, method }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString() }));
    });
    req.on('error', reject);
    req.end();
  });
}

test('Fall 2025 REST endpoints are served from memory', async () => {
  const { pub, gw, port, cleanup } = await setup();
  try {
    pub.publish([
      trackUpdate({ icao: 0x400001, alt: 1200, lat: 40.1, lon: -88.2, mask: 1, callsign: 'HELI1' }),
      trackUpdate({ icao: 0x400001, alt: 1300, lat: 40.11, lon: -88.21, mask: 1, callsign: 'HELI1' }),
      trackUpdate({ icao: 0x400002, alt: 35000, lat: 40.5, lon: -88.5, mask: 1, callsign: 'UAL1' }),
      trackUpdate({ icao: 0x400003, alt: 5000 }), // no position: not on the map
    ]);
    await waitFor(() => gw.core.expected === 5, 2000, 'applied');
    const ac = JSON.parse((await get(port, '/api/aircraft?window=0')).body);
    assert.equal(ac.aircraft.length, 2);
    const heli = ac.aircraft.find((a) => a.hex === '400001');
    assert.equal(heli.flight, 'HELI1');
    assert.equal(heli.altitude, 1300);
    assert.equal(heli.type, 6, 'aircraft type from the map directory');
    assert.ok(Number.isInteger(heli.seen));
    const trail = JSON.parse((await get(port, '/api/aircraft/400001/trail?limit=10')).body);
    assert.equal(trail.count, 2);
    assert.equal(trail.trail[1].altitude, 1300);
    const stats = JSON.parse((await get(port, '/api/statistics')).body);
    assert.equal(stats.summary.total_flights, 2);
    assert.deepEqual(
      stats.altitude_distribution.map((d) => [d.range, d.count]),
      [['0-10k', 1], ['30-40k', 1]],
    );
    assert.equal(stats.flight_durations.length, 2);
    assert.equal(JSON.parse((await get(port, '/api/health')).body).status, 'ok');
    assert.deepEqual(JSON.parse((await get(port, '/api/collisions/kalman')).body).collisions, []);
    const enriched = JSON.parse((await get(port, '/api/aircraft/enriched')).body);
    assert.equal(enriched.aircraft.find((a) => a.hex === '400001').trail.length, 2);
    assert.equal((await get(port, '/api/unknown')).status, 404);
    const st = JSON.parse((await get(port, '/gateway/stats')).body);
    assert.equal(st.subscriber.applied, 4);
    const dg = JSON.parse((await get(port, '/gateway/digest')).body);
    assert.equal(dg.digest, pub.pub.book.hexDigest());
    // A delete removes the aircraft and its trail.
    pub.publish([trackDelete(0x400001)]);
    await waitFor(() => gw.core.expected === 6, 2000, 'delete');
    assert.equal(JSON.parse((await get(port, '/api/aircraft/400001/trail')).body).count, 0);
  } finally {
    await cleanup();
  }
});

test('static files: served as they are, dotfiles and traversal refused', async () => {
  const { port, cleanup } = await setup();
  try {
    const index = await get(port, '/');
    assert.equal(index.status, 200);
    // The page is served unchanged: app.js itself takes the live feed.
    assert.doesNotMatch(index.body, /map-shim/);
    assert.equal((await get(port, '/app.js')).status, 200);
    assert.equal((await get(port, '/gateway/map-shim.js')).status, 404);
    assert.equal((await get(port, '/gateway/gateway_protocol.mjs')).status, 200);
    // Each target exists and has a served file type, so only the path rules stop it.
    for (const bad of ['/.env', '/%2eenv', '/.hidden.json', '/%2ehidden.json', '/../secret.json', '/%2e%2e/secret.json', '/..%2fsecret.json', '/%2e%2e%5csecret.json', '/x.exe', '/%E0%A4%A']) {
      assert.equal((await get(port, bad)).status, 404, bad);
    }
    assert.equal((await get(port, '/', 'POST')).status, 405);
    assert.equal((await get(port, '/', 'HEAD')).status, 200);
  } finally {
    await cleanup();
  }
});

