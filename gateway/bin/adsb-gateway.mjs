#!/usr/bin/env node
// adsb-gateway: subscribe to the track feed and serve it to browsers over
// WebSockets, together with the Fall 2025 map (gateway/README.md).

import { existsSync } from 'node:fs';
import { join } from 'node:path';

import { Gateway, defaultGatewayOptions, repoRoot } from '../src/gateway.mjs';
import { parseEndpoint } from '../src/feed_client.mjs';

const usage = `usage: adsb-gateway [options]
  --feed HOST:PORT        multicast group or local unicast address (default 239.19.0.1:41900)
  --interface ADDR        interface to join the group on (default: kernel's choice)
  --retransmit HOST:PORT  retransmit server (default 127.0.0.1:41901)
  --snapshot HOST:PORT    snapshot server (default 127.0.0.1:41902)
  --listen HOST:PORT      HTTP + WebSocket address (default 127.0.0.1:8080)
  --map-dir DIR           Fall 2025 map to serve (default: <repo>/ads)
  --flush-ms N            batch WebSocket frames for N ms (default 0: send as soon as possible)
  --high-water BYTES      skip a client with more than this unsent (default 0: write a frame only
                          once the previous one is in the kernel, so slow clients get batched frames)
  --stuck-ms N            disconnect a client stuck over the high-water mark this long (default 10000)
  --max-clients N         (default 256)
  --trail-points N        positions kept per aircraft for /api/aircraft/:hex/trail (default 200)
  --clock-helper PATH     adsb-clock binary, to calibrate latency against the publisher's clock
                          (default: core/build/{release,debug}/tools/adsb-clock if built; "none" to skip)
  --window N              subscriber buffer, messages (default 16384)
  --max-request N         messages per retransmit request (default 1024)
  --drop-rate P --drop-burst N --seed N --drop-only-first N
                          drop received feed datagrams (fault injection)
  --until-end             exit once the session has ended, everything is recovered, and
                          every client has been sent the final state
  --duration S            exit after S seconds
  --stats-every S         print a stats line every S seconds (default 0: off)
  --quiet                 only the final lines
The last line printed on exit is the book digest (same as adsb-publish's "book digest").`;

const args = process.argv.slice(2);
const opt = {};
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  const val = () => {
    if (i + 1 >= args.length) {
      console.error(`${a} needs a value\n${usage}`);
      process.exit(2);
    }
    return args[++i];
  };
  switch (a) {
    case '--feed': opt.feed = parseEndpoint(val()); break;
    case '--interface': opt.interfaceHost = val(); break;
    case '--retransmit': opt.retransmit = parseEndpoint(val()); break;
    case '--snapshot': opt.snapshot = parseEndpoint(val()); break;
    case '--listen': opt.listen = parseEndpoint(val()); break;
    case '--map-dir': opt.mapDir = val(); break;
    case '--flush-ms': opt.flushMs = Number(val()); break;
    case '--high-water': opt.highWater = Number(val()); break;
    case '--stuck-ms': opt.stuckMs = Number(val()); break;
    case '--max-clients': opt.maxClients = Number(val()); break;
    case '--trail-points': opt.trailPoints = Number(val()); break;
    case '--clock-helper': opt.clockHelper = val(); break;
    case '--window': opt.window = Number(val()); break;
    case '--max-request': opt.maxRequest = Number(val()); break;
    case '--drop-rate': opt.dropRate = Number(val()); break;
    case '--drop-burst': opt.dropBurst = Number(val()); break;
    case '--seed': opt.seed = Number(val()); break;
    case '--drop-only-first': opt.onlyFirst = Number(val()); break;
    case '--until-end': opt.untilEnd = true; break;
    case '--duration': opt.duration = Number(val()); break;
    case '--stats-every': opt.statsEvery = Number(val()); break;
    case '--quiet': opt.quiet = true; break;
    case '-h': case '--help': console.log(usage); process.exit(0);
    default: console.error(`unknown option ${a}\n${usage}`); process.exit(2);
  }
}

const positive = (name, v, max = Infinity) => {
  if (v === undefined) return;
  if (!Number.isFinite(v) || v < 0 || v > max) {
    console.error(`${name}: expected a number in [0, ${max}]`);
    process.exit(2);
  }
};
positive('--flush-ms', opt.flushMs, 10_000);
positive('--window', opt.window, 1 << 20);
positive('--max-request', opt.maxRequest, 65535);
positive('--drop-rate', opt.dropRate, 1);

let clockHelper = opt.clockHelper;
if (clockHelper === undefined) {
  clockHelper = ['release', 'debug']
    .map((p) => join(repoRoot, 'core', 'build', p, 'tools', 'adsb-clock'))
    .find((p) => existsSync(p)) ?? null;
} else if (clockHelper === 'none') clockHelper = null;

const feedOptions = {
  ...(opt.feed && { feed: opt.feed }),
  ...(opt.interfaceHost && { interfaceHost: opt.interfaceHost }),
  ...(opt.retransmit && { retransmit: opt.retransmit }),
  ...(opt.snapshot && { snapshot: opt.snapshot }),
  loss: { dropRate: opt.dropRate ?? 0, dropBurst: opt.dropBurst ?? 1, seed: opt.seed ?? 1, onlyFirst: opt.onlyFirst ?? 0 },
  core: { ...(opt.window && { window: opt.window }), ...(opt.maxRequest && { maxRequest: opt.maxRequest }) },
};

const gw = new Gateway({
  ...(opt.listen && { listen: opt.listen }),
  ...(opt.mapDir && { mapDir: opt.mapDir }),
  ...(opt.flushMs !== undefined && { flushMs: opt.flushMs }),
  ...(opt.highWater !== undefined && { highWater: opt.highWater }),
  ...(opt.stuckMs !== undefined && { stuckMs: opt.stuckMs }),
  ...(opt.maxClients !== undefined && { maxClients: opt.maxClients }),
  ...(opt.trailPoints !== undefined && { trailPoints: opt.trailPoints }),
  clockHelper,
  feedOptions,
});

const log = (...a) => {
  if (!opt.quiet) console.log(...a);
};

try {
  await gw.start();
} catch (e) {
  console.error(`adsb-gateway: ${e.message}`);
  process.exit(1);
}
const addr = gw.address();
const cfg = gw.feed.opts;
log(`feed ${cfg.feed.host}:${cfg.feed.port}  retransmit ${cfg.retransmit.host}:${cfg.retransmit.port}  snapshot ${cfg.snapshot.host}:${cfg.snapshot.port}`);
log(`map  http://${addr.address}:${addr.port}/   websocket ws://${addr.address}:${addr.port}/gateway/ws   stats /gateway/stats`);
const ck = gw.clock.describe();
log(ck.calibrated ? `clock offset to publisher ${ck.offset_ns} ns (±${ck.uncertainty_ns} ns) via ${clockHelper}` : 'clock: uncalibrated (no adsb-clock helper)');

const fmt = (h) => (h.count ? `p50 ${h.p50_us} p99 ${h.p99_us} p99.9 ${h.p999_us} max ${h.max_us} µs (n=${h.count})` : 'n=0');

function report(final) {
  const s = gw.stats();
  const sub = s.subscriber;
  const line =
    `seq ${s.status.as_of_seq} tracks ${s.status.tracks} mode ${s.status.mode} clients ${s.status.clients} | ` +
    `applied ${sub.applied} requests ${sub.requests} naks ${sub.naks} snapshots ${sub.snapshots_loaded} dropped ${sub.feed_dropped} | ` +
    `frames ${s.fanout.frames} sent ${s.fanout.messages_sent} conflated ${s.fanout.conflated}`;
  if (final || !opt.quiet) console.log(line);
  if (final) {
    console.log(`publish -> gateway book:  ${fmt(s.latency_us.feed_to_gateway)}${s.clock.calibrated ? '' : ' [uncalibrated]'}`);
    console.log(`publish -> websocket out: ${fmt(s.latency_us.feed_to_ws)}${s.clock.calibrated ? '' : ' [uncalibrated]'}`);
    console.log(`subscriber ${JSON.stringify(sub)}`);
  }
}

let statsTimer = null;
if (opt.statsEvery > 0) statsTimer = setInterval(() => report(false), opt.statsEvery * 1000);

let stopping = false;
async function shutdown(code) {
  if (stopping) return;
  stopping = true;
  clearInterval(statsTimer);
  report(true);
  const d = gw.digest();
  await gw.stop();
  console.log(`session ${d.session} as_of_seq ${d.as_of_seq} tracks ${d.tracks} ended ${d.ended}`);
  console.log(`book digest ${d.digest}`);
  process.exit(code);
}

process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
if (opt.duration > 0) setTimeout(() => shutdown(opt.untilEnd ? 3 : 0), opt.duration * 1000);

if (opt.untilEnd) {
  await gw.feed.untilEnded();
  // Let every connected client receive the final frame.
  const drained = () =>
    [...gw.clients].every(
      (c) => !c.needSnapshot && c.dirty.size === 0 && c.sentFlags === gw.frameFlags() && c.sentSeq === gw.asOfSeq() && c.ws.bufferedBytes === 0,
    );
  const t0 = Date.now();
  while (!drained() && Date.now() - t0 < 20000) await new Promise((r) => setTimeout(r, 10));
  await new Promise((r) => setTimeout(r, 200));
  await shutdown(0);
}
