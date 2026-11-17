// Network fixtures: a JS publisher on real sockets (UDP feed, UDP retransmit
// server, TCP snapshot server) and a raw WebSocket client that can stop
// reading, to play a slow browser.

import { randomBytes } from 'node:crypto';
import dgram from 'node:dgram';
import net from 'node:net';

import { decodeFrame } from '../public/gateway_protocol.mjs';
import { FrameDecoder } from '../src/websocket.mjs';
import { SimPublisher } from './helpers.mjs';

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function waitFor(cond, timeoutMs = 5000, what = 'condition') {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > timeoutMs) throw new Error(`timed out waiting for ${what}`);
    await sleep(5);
  }
}

export class NetPublisher {
  constructor(session, { ring = 4096, maxReply = 4096, dropRate = 0, seed = 1 } = {}) {
    this.pub = new SimPublisher(session, { ring, maxReply });
    this.dropRate = dropRate;
    this.rngState = seed;
    this.dest = null;
    this.requests = 0;
    this.snapshots = 0;
  }

  rand() {
    this.rngState = (this.rngState * 1103515245 + 12345) >>> 0;
    return this.rngState / 2 ** 32;
  }

  async open() {
    this.feedSock = dgram.createSocket('udp4');
    this.rtxSock = dgram.createSocket('udp4');
    await new Promise((r) => this.feedSock.bind(0, '127.0.0.1', r));
    await new Promise((r) => this.rtxSock.bind(0, '127.0.0.1', r));
    this.rtxSock.on('message', (msg, rinfo) => {
      this.requests++;
      for (const reply of this.pub.handleRequest(msg)) this.rtxSock.send(reply, rinfo.port, rinfo.address);
    });
    this.snapServer = net.createServer((sock) => {
      this.snapshots++;
      sock.end(this.pub.snapshotBytes());
    });
    await new Promise((r) => this.snapServer.listen(0, '127.0.0.1', r));
    return this;
  }

  get retransmit() {
    return { host: '127.0.0.1', port: this.rtxSock.address().port };
  }

  get snapshot() {
    return { host: '127.0.0.1', port: this.snapServer.address().port };
  }

  // A new session on the same sockets (publisher restart).
  restart(session) {
    this.send(this.pub.heartbeat(true));
    const { ring, maxReply } = this.pub;
    this.pub = new SimPublisher(session, { ring, maxReply });
  }

  send(pkt) {
    if (!this.dest) return;
    if (this.dropRate > 0 && this.rand() < this.dropRate) return;
    this.feedSock.send(pkt, this.dest.port, this.dest.host);
  }

  // Publishes messages, `perPacket` to a datagram.
  publish(msgs, perPacket = 5) {
    for (let i = 0; i < msgs.length; i += perPacket) {
      for (const m of msgs.slice(i, i + perPacket)) this.pub.publish(m);
      this.send(this.pub.flush());
    }
  }

  heartbeat(eos = false) {
    this.send(this.pub.heartbeat(eos));
  }

  close() {
    this.feedSock.close();
    this.rtxSock.close();
    this.snapServer.close();
  }
}

export function maskedFrame(opcode, payload, fin = true) {
  const mask = randomBytes(4);
  const len = payload.length;
  let head;
  if (len < 126) head = Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | len]);
  else if (len < 65536) {
    head = Buffer.alloc(4);
    head[0] = (fin ? 0x80 : 0) | opcode;
    head[1] = 0x80 | 126;
    head.writeUInt16BE(len, 2);
  } else {
    head = Buffer.alloc(10);
    head[0] = (fin ? 0x80 : 0) | opcode;
    head[1] = 0x80 | 127;
    head.writeBigUInt64BE(BigInt(len), 2);
  }
  const body = Buffer.from(payload);
  for (let i = 0; i < body.length; i++) body[i] ^= mask[i & 3];
  return Buffer.concat([head, mask, body]);
}

// A WebSocket client on a raw TCP socket. `pause()` stops reading, so the
// server's socket buffer fills like a stalled browser tab's.
export class RawClient {
  constructor(port, path = '/gateway/ws', headers = {}) {
    this.port = port;
    this.path = path;
    this.headers = headers;
    this.frames = []; // decoded gateway frames
    this.texts = [];
    this.raw = []; // { opcode, payload } of every server frame
    this.decoder = new FrameDecoder({ requireMask: false, maxPayload: 1 << 30 });
    this.closed = false;
    this.response = '';
  }

  connect() {
    return new Promise((resolve, reject) => {
      const s = (this.socket = net.connect(this.port, '127.0.0.1'));
      const key = randomBytes(16).toString('base64');
      const extra = Object.entries(this.headers)
        .map(([k, v]) => `${k}: ${v}\r\n`)
        .join('');
      s.on('connect', () =>
        s.write(
          `GET ${this.path} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
            `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n${extra}\r\n`,
        ),
      );
      let head = Buffer.alloc(0);
      let upgraded = false;
      s.on('data', (chunk) => {
        if (!upgraded) {
          head = Buffer.concat([head, chunk]);
          const end = head.indexOf('\r\n\r\n');
          if (end < 0) return;
          this.response = head.subarray(0, end).toString();
          upgraded = true;
          if (!this.response.startsWith('HTTP/1.1 101')) {
            resolve(this);
            return;
          }
          resolve(this);
          chunk = head.subarray(end + 4);
        }
        for (const f of this.decoder.push(chunk)) {
          this.raw.push(f);
          if (f.opcode === 2) this.frames.push(decodeFrame(f.payload));
          else if (f.opcode === 1) this.texts.push(f.payload.toString());
        }
      });
      s.on('close', () => {
        this.closed = true;
        if (!upgraded) resolve(this);
      });
      s.on('error', (e) => (upgraded ? null : reject(e)));
    });
  }

  get upgraded() {
    return this.response.startsWith('HTTP/1.1 101');
  }

  send(buf) {
    this.socket.write(buf);
  }

  pause() {
    this.socket.pause();
  }

  resume() {
    this.socket.resume();
  }

  close() {
    this.socket.destroy();
  }
}
