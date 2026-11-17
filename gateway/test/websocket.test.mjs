// The WebSocket server: handshake, framing, control frames, limits, and
// interoperability with Node's own (browser-compatible) WebSocket client.

import assert from 'node:assert/strict';
import http from 'node:http';
import { test } from 'node:test';

import { FrameDecoder, Opcode, acceptKey, acceptUpgrade, frameHeader } from '../src/websocket.mjs';
import { RawClient, maskedFrame, sleep, waitFor } from './net_fixture.mjs';

test('accept key matches the RFC 6455 example', () => {
  assert.equal(acceptKey('dGhlIHNhbXBsZSBub25jZQ=='), 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=');
});

test('frame headers use the shortest length encoding', () => {
  assert.deepEqual([...frameHeader(Opcode.kBinary, 125)], [0x82, 125]);
  assert.deepEqual([...frameHeader(Opcode.kBinary, 126)], [0x82, 126, 0, 126]);
  assert.deepEqual([...frameHeader(Opcode.kText, 65535)], [0x81, 126, 0xff, 0xff]);
  const h = frameHeader(Opcode.kBinary, 65536);
  assert.equal(h.length, 10);
  assert.equal(h[1], 127);
  assert.equal(h.readBigUInt64BE(2), 65536n);
});

test('decoder: masked frames in any chunking, all length forms', () => {
  for (const size of [0, 5, 125, 126, 300, 65535, 65536]) {
    const payload = Buffer.alloc(size, 7);
    const wire = maskedFrame(Opcode.kBinary, payload);
    for (const chunk of [1, 3, 1000, wire.length]) {
      const d = new FrameDecoder({ maxPayload: 1 << 20 });
      const frames = [];
      for (let off = 0; off < wire.length; off += chunk) frames.push(...d.push(wire.subarray(off, off + chunk)));
      assert.equal(frames.length, 1, `size ${size} chunk ${chunk}`);
      assert.ok(frames[0].payload.equals(payload));
    }
  }
});

test('decoder rejects protocol violations', () => {
  const code = (bytes, opts) => {
    try {
      new FrameDecoder(opts).push(bytes);
      return null;
    } catch (e) {
      return e.code;
    }
  };
  assert.equal(code(Buffer.from([0x82, 0x00])), 1002, 'unmasked client frame');
  assert.equal(code(Buffer.from([0xc2, 0x80, 0, 0, 0, 0])), 1002, 'reserved bit');
  assert.equal(code(Buffer.from([0x83, 0x80, 0, 0, 0, 0])), 1002, 'unknown opcode');
  assert.equal(code(maskedFrame(Opcode.kPing, Buffer.alloc(126))), 1002, 'control frame > 125 bytes');
  assert.equal(code(maskedFrame(Opcode.kPing, Buffer.alloc(1), false)), 1002, 'fragmented control frame');
  assert.equal(code(maskedFrame(Opcode.kBinary, Buffer.alloc(100)), { maxPayload: 99 }), 1009, 'too big');
  const huge = Buffer.from([0x82, 0xff, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
  assert.equal(code(huge), 1009, '64-bit length beyond the cap');
});

async function echoServer() {
  const server = http.createServer();
  const conns = [];
  server.on('upgrade', (req, socket, head) => {
    const ws = acceptUpgrade(req, socket, head);
    if (!ws) return;
    conns.push(ws);
    ws.on('message', (data, binary) => {
      if (binary) ws.sendBinary([data]);
      else ws.sendText(`echo:${data.toString()}`);
    });
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  return { server, conns, port: server.address().port };
}

test("interoperates with Node's WebSocket client (text, binary, close)", async () => {
  const { server, port, conns } = await echoServer();
  const ws = new WebSocket(`ws://127.0.0.1:${port}/`);
  ws.binaryType = 'arraybuffer';
  const got = [];
  ws.onmessage = (e) => got.push(e.data);
  await new Promise((r) => (ws.onopen = r));
  ws.send('héllo');
  ws.send(new Uint8Array(60000).fill(3));
  await waitFor(() => got.length === 2, 3000, 'echoes');
  assert.equal(got[0], 'echo:héllo');
  assert.equal(got[1].byteLength, 60000);
  const closed = new Promise((r) => (ws.onclose = (e) => r(e.code)));
  ws.close(1000);
  assert.equal(await closed, 1000);
  await waitFor(() => !conns[0].open, 1000, 'server side closed');
  // Over the 64 KiB client-message cap: closed with 1009.
  const big = new WebSocket(`ws://127.0.0.1:${port}/`);
  await new Promise((r) => (big.onopen = r));
  const bigClosed = new Promise((r) => (big.onclose = (e) => r(e.code)));
  big.send(new Uint8Array(70000));
  assert.equal(await bigClosed, 1009);
  server.close();
});

test('ping is answered with a pong carrying the same payload', async () => {
  const { server, port } = await echoServer();
  const c = await new RawClient(port, '/').connect();
  c.send(maskedFrame(Opcode.kPing, Buffer.from('abc')));
  await waitFor(() => c.raw.some((f) => f.opcode === Opcode.kPong), 2000, 'pong');
  assert.equal(c.raw.find((f) => f.opcode === Opcode.kPong).payload.toString(), 'abc');
  c.close();
  server.close();
});

test('fragmented messages are reassembled, with control frames in between', async () => {
  const { server, port } = await echoServer();
  const c = await new RawClient(port, '/').connect();
  c.send(maskedFrame(Opcode.kText, Buffer.from('ab'), false));
  c.send(maskedFrame(Opcode.kPing, Buffer.from('p')));
  c.send(maskedFrame(Opcode.kCont, Buffer.from('cd'), true));
  await waitFor(() => c.texts.length === 1, 2000, 'echo');
  assert.equal(c.texts[0], 'echo:abcd');
  c.close();
  server.close();
});

test('invalid UTF-8 text closes with 1007; unmasked frames close with 1002', async () => {
  const { server, port } = await echoServer();
  const a = await new RawClient(port, '/').connect();
  a.send(maskedFrame(Opcode.kText, Buffer.from([0xc3, 0x28])));
  await waitFor(() => a.raw.some((f) => f.opcode === Opcode.kClose), 2000, 'close');
  assert.equal(a.raw.find((f) => f.opcode === Opcode.kClose).payload.readUInt16BE(0), 1007);
  const b = await new RawClient(port, '/').connect();
  b.send(Buffer.from([0x82, 0x01, 0x00]));
  await waitFor(() => b.raw.some((f) => f.opcode === Opcode.kClose), 2000, 'close');
  assert.equal(b.raw.find((f) => f.opcode === Opcode.kClose).payload.readUInt16BE(0), 1002);
  a.close();
  b.close();
  server.close();
});

test('bad upgrade requests are refused', async () => {
  const { server, port } = await echoServer();
  const v = await new RawClient(port, '/', { 'X-Ignore': '1' }).connect();
  assert.ok(v.upgraded);
  v.close();
  // Wrong version.
  const res = await new Promise((resolve) => {
    const req = http.request({
      port,
      host: '127.0.0.1',
      headers: { Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==', 'Sec-WebSocket-Version': '8' },
    });
    req.on('upgrade', () => resolve('upgraded'));
    req.on('response', (r) => resolve(r.statusCode));
    req.end();
  });
  assert.equal(res, 426);
  const bad = await new Promise((resolve) => {
    const req = http.request({
      port,
      host: '127.0.0.1',
      headers: { Connection: 'Upgrade', Upgrade: 'websocket', 'Sec-WebSocket-Key': 'short', 'Sec-WebSocket-Version': '13' },
    });
    req.on('upgrade', () => resolve('upgraded'));
    req.on('response', (r) => resolve(r.statusCode));
    req.end();
  });
  assert.equal(bad, 400);
  await sleep(10);
  server.close();
});

test('close codes: valid ones are echoed, invalid ones answered with 1002', async () => {
  const { server, port } = await echoServer();
  const closeWith = async (payload) => {
    const c = await new RawClient(port, '/').connect();
    c.send(maskedFrame(Opcode.kClose, payload));
    await waitFor(() => c.raw.some((f) => f.opcode === Opcode.kClose), 2000, 'close');
    const p = c.raw.find((f) => f.opcode === Opcode.kClose).payload;
    c.close();
    return p.length >= 2 ? p.readUInt16BE(0) : null;
  };
  const code = (n) => {
    const b = Buffer.alloc(2);
    b.writeUInt16BE(n);
    return b;
  };
  assert.equal(await closeWith(Buffer.alloc(0)), 1000);
  assert.equal(await closeWith(code(1001)), 1001);
  assert.equal(await closeWith(code(4000)), 4000);
  assert.equal(await closeWith(code(999)), 1002);
  assert.equal(await closeWith(code(1005)), 1002);
  assert.equal(await closeWith(code(1015)), 1002);
  assert.equal(await closeWith(Buffer.from([3])), 1002, 'one-byte close payload');
  server.close();
});

test('a peer that half-closes (FIN, no close frame) is closed', async () => {
  const { server, port, conns } = await echoServer();
  const c = await new RawClient(port, '/').connect();
  await waitFor(() => conns.length === 1, 2000, 'server side open');
  c.socket.end();
  await waitFor(() => !conns[0].open, 2000, 'server closed its side');
  server.close();
});
