// The map pages' live feed (ads/live-feed.js), run as a browser would run it:
// the script in a vm context with a window, location and the platform
// WebSocket, against a real Gateway fed by a NetPublisher.

import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';
import vm from 'node:vm';

import { FrameFlags, toAircraft } from '../public/gateway_protocol.mjs';
import { bookDigest } from '../src/feed.mjs';
import { Gateway } from '../src/gateway.mjs';
import { NetPublisher, waitFor } from './net_fixture.mjs';
import { Rng, S1, trackDelete, trackUpdate } from './helpers.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const adsDir = join(here, '..', '..', 'ads');
const liveFeedJs = readFileSync(join(adsDir, 'live-feed.js'), 'utf8');
const protocolUrl = pathToFileURL(join(here, '..', 'public', 'gateway_protocol.mjs')).href;
const tableDigest = (tracks) => bookDigest(new Map([...tracks].map(([k, v]) => [k, Buffer.from(v)])));

function traffic(n, { seed, ts0 = 1, icaos = 60 }) {
  const rng = new Rng(seed);
  const out = [];
  for (let i = 0; i < n; i++) {
    const icao = 0x400000 + rng.below(icaos);
    out.push(trackUpdate({ icao, alt: rng.below(40000), lat: 40 + rng.next(), lon: -88 - rng.next(), mask: 1 | 2, eventTs: ts0 + i }));
  }
  return out;
}

// A page: the script evaluated in its own context, the way a <script> tag runs it.
function page(env, { search = '', protocol = protocolUrl, renderMs } = {}) {
  const port = env.port;
  const p = { renders: [], statuses: [], unavailable: false };
  const window = {};
  const context = vm.createContext({
    window,
    location: { protocol: 'http:', host: `127.0.0.1:${port}`, search },
    WebSocket,
    URLSearchParams,
    performance,
    setTimeout,
    clearTimeout,
    requestAnimationFrame: (cb) => setTimeout(() => cb(performance.now()), 0),
    console,
  });
  new vm.Script(liveFeedJs, {
    filename: 'live-feed.js',
    importModuleDynamically: vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER,
  }).runInContext(context);
  p.feed = window.startLiveFeed({
    protocolUrl: protocol,
    renderMs,
    onRender: (aircraft, feed) => p.renders.push({ aircraft, asOfSeq: feed.asOfSeq, at: performance.now() }),
    onStatus: (ok, text) => p.statuses.push(text),
    onUnavailable: () => {
      p.unavailable = true;
    },
  });
  assert.equal(window.__gateway, p.feed);
  env.pages.push(p);
  return p;
}

async function setup(listenPort = 0) {
  const pub = await new NetPublisher(S1, {}).open();
  const base = mkdtempSync(join(tmpdir(), 'gw-live-'));
  const mapDir = join(base, 'map');
  mkdirSync(mapDir);
  const start = async (port) => {
    const gw = new Gateway({
      listen: { host: '127.0.0.1', port },
      mapDir,
      clockHelper: null,
      statusMs: 50,
      feedOptions: { feed: { host: '127.0.0.1', port: 0 }, retransmit: pub.retransmit, snapshot: pub.snapshot, pollMs: 1 },
    });
    await gw.start();
    pub.dest = { host: '127.0.0.1', port: gw.feed.feedPort() };
    return gw;
  };
  const env = { pub, gw: await start(listenPort), start, pages: [] };
  env.port = env.gw.address().port;
  env.cleanup = async () => {
    for (const p of env.pages) p.feed.stop();
    await env.gw.stop().catch(() => {});
    pub.close();
    rmSync(base, { recursive: true, force: true });
  };
  return env;
}

const lastRender = (p) => p.renders[p.renders.length - 1];

// What a page should draw: every track of the publisher's book with a position.
function expected(pub) {
  const out = new Map();
  for (const [, u] of pub.pub.book.tracks) {
    const a = toAircraft(new Uint8Array(u));
    if (a.lat !== null && a.lon !== null) out.set(a.hex, a);
  }
  return out;
}

test('a page draws the gateway table: every aircraft, at its latest position, at a bounded rate', async () => {
  const env = await setup();
  try {
    env.pub.publish(traffic(400, { seed: 1 }));
    await waitFor(() => env.gw.core.expected === 401, 3000, 'gateway applies');
    const p = page(env);
    await waitFor(() => p.feed.synced && p.renders.length > 0, 3000, 'first render');
    assert.equal(p.statuses.at(-1), 'Live (WebSocket)'); // reported by that render
    // More updates arrive in many frames; the page redraws at most every
    // render_ms (100 ms) and its last drawing is the publisher's book.
    const before = p.renders.length;
    const t0 = performance.now();
    for (let k = 0; k < 20; k++) {
      env.pub.publish(traffic(50, { seed: 10 + k, ts0: 1000 + k * 50 }));
      await new Promise((r) => setTimeout(r, 15));
    }
    // Some aircraft leave: they must leave the drawing and the page's state.
    for (const icao of [0x400001, 0x400002, 0x400003]) assert.ok(env.pub.pub.book.tracks.has(icao), 'present before');
    env.pub.publish([0x400001, 0x400002, 0x400003].map((icao) => trackDelete(icao, 99999)));
    env.pub.heartbeat();
    const want = () => expected(env.pub);
    await waitFor(() => {
      const r = lastRender(p);
      return r && r.asOfSeq === env.pub.pub.next - 1 && r.aircraft.length === want().size;
    }, 3000, 'page shows the final table');
    const elapsed = performance.now() - t0;
    const r = lastRender(p);
    const w = want();
    for (const a of r.aircraft) {
      const e = w.get(a.hex);
      assert.ok(e, `${a.hex} is in the book`);
      assert.equal(a.lat, e.lat);
      assert.equal(a.lon, e.lon);
      assert.equal(a.altitude, e.altitude);
      assert.ok(a.seen !== null, 'seen is set');
    }
    assert.equal(tableDigest(p.feed.table.tracks), env.pub.pub.book.hexDigest());
    assert.ok(!r.aircraft.some((a) => ['400001', '400002', '400003'].includes(a.hex)), 'deleted aircraft are gone');
    assert.deepEqual([...p.feed.seen.keys()].sort(), [...p.feed.table.tracks.keys()].sort(), 'seen tracks the table');
    assert.ok(p.feed.frames > p.renders.length - before, 'frames are batched into renders');
    const renders = p.renders.length - before;
    assert.ok(renders <= Math.ceil(elapsed / 100) + 2, `${renders} renders in ${elapsed.toFixed(0)} ms`);
    for (let i = before + 1; i < p.renders.length; i++) {
      assert.ok(p.renders[i].at - p.renders[i - 1].at >= 95, 'renders at least render_ms apart');
    }
    assert.ok(p.feed.renderTimeMs >= 0 && p.feed.maxRenderTimeMs >= p.feed.renderTimeMs);
  } finally {
    await env.cleanup();
  }
});

test('?render_ms and the renderMs option set the interval', async () => {
  const env = await setup();
  try {
    assert.equal(page(env, { renderMs: 250 }).feed.renderMs, 250);
    assert.equal(page(env, { renderMs: 250, search: '?render_ms=40' }).feed.renderMs, 40);
    assert.equal(page(env, { search: '?animate=1' }).feed.animate, true);
    assert.equal(page(env).feed.renderMs, 100);
  } finally {
    await env.cleanup();
  }
});

test('flow control: a page acknowledges frames, so a long burst never stalls it', async () => {
  const env = await setup();
  try {
    const p = page(env);
    await waitFor(() => p.feed.synced, 3000, 'sync');
    // Far more frames than the window of 8: without acks the gateway would
    // stop sending after 8 and the page would never see the last sequence.
    for (let k = 0; k < 60; k++) {
      env.pub.publish(traffic(5, { seed: 100 + k, ts0: 5000 + k * 5 }));
      await new Promise((r) => setTimeout(r, 5));
    }
    env.pub.heartbeat();
    await waitFor(() => p.feed.asOfSeq === env.pub.pub.next - 1, 3000, 'page reaches the last sequence');
    assert.ok(p.feed.frames > 8, `${p.feed.frames} frames`);
    assert.equal(tableDigest(p.feed.table.tracks), env.pub.pub.book.hexDigest());
  } finally {
    await env.cleanup();
  }
});

test('the gateway restarts: the page reports it, reconnects and resyncs', async () => {
  const env = await setup();
  try {
    env.pub.publish(traffic(100, { seed: 3 }));
    const p = page(env);
    await waitFor(() => p.feed.synced && p.renders.length > 0, 3000, 'sync');
    await env.gw.stop();
    await waitFor(() => !p.feed.connected && !p.feed.synced, 3000, 'page sees the close');
    assert.equal(p.statuses.at(-1), 'Gateway disconnected');
    env.gw = await env.start(env.port); // same address
    env.pub.publish(traffic(30, { seed: 4, ts0: 2000 }));
    env.pub.heartbeat();
    await waitFor(() => p.feed.synced && p.feed.asOfSeq === env.pub.pub.next - 1, 8000, 'resynced');
    assert.ok(p.feed.reconnects >= 1);
    // The status is reported with the next redraw (within render_ms).
    await waitFor(() => p.statuses.at(-1) === 'Live (WebSocket)', 2000, 'live status');
    assert.equal(tableDigest(p.feed.table.tracks), env.pub.pub.book.hexDigest());
  } finally {
    await env.cleanup();
  }
});

test('the end of the session is reported', async () => {
  const env = await setup();
  try {
    env.pub.publish(traffic(20, { seed: 5 }));
    const p = page(env);
    await waitFor(() => p.feed.synced, 3000, 'sync');
    env.pub.heartbeat(true);
    await waitFor(() => (p.feed.flags & FrameFlags.kEnded) !== 0, 3000, 'ended flag');
    await waitFor(() => p.statuses.at(-1) === 'Feed ended', 2000, 'status');
  } finally {
    await env.cleanup();
  }
});

test('without a gateway (the Fall 2025 server) the feed reports it is unavailable', async () => {
  const env = await setup();
  try {
    const p = page(env, { protocol: pathToFileURL(join(tmpdir(), 'no-such-module.mjs')).href });
    await waitFor(() => p.unavailable, 2000, 'onUnavailable');
    assert.equal(p.feed.available, false);
    assert.equal(p.feed.connected, false);
    assert.equal(p.renders.length, 0);
  } finally {
    await env.cleanup();
  }
});

test('both map pages load the live feed before their own script, and use it', () => {
  for (const [html, js, start] of [
    ['index.html', 'app.js', 'startLiveFeed2D'],
    ['3d.html', 'cesium-app.js', 'startLiveFeed3D'],
  ]) {
    const h = readFileSync(join(adsDir, html), 'utf8');
    const feedAt = h.indexOf('<script src="live-feed.js"></script>');
    assert.ok(feedAt >= 0, `${html} loads live-feed.js`);
    assert.ok(feedAt < h.indexOf(js), `${html}: live-feed.js before ${js}`);
    const src = readFileSync(join(adsDir, js), 'utf8');
    assert.match(src, new RegExp(`window\\.startLiveFeed\\(`), `${js} starts the feed`);
    assert.match(src, new RegExp(`${start}\\(\\);`), `${js} calls ${start}`);
  }
});
