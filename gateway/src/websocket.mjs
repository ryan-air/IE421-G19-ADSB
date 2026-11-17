// Minimal WebSocket server (RFC 6455), no dependencies.
//
// Enough for the gateway: the opening handshake on an HTTP upgrade, unmasked
// server frames (binary and text), masked client frames (text, binary, ping,
// pong, close, continuation), and a clean close. Client messages are capped
// at 64 KiB: browsers only send small control messages here. Any protocol
// violation closes the connection with the matching status code.

import { createHash } from 'node:crypto';
import { EventEmitter } from 'node:events';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
export const kMaxClientMessage = 64 * 1024;

export const Opcode = Object.freeze({ kCont: 0, kText: 1, kBinary: 2, kClose: 8, kPing: 9, kPong: 10 });

export function isValidCloseCode(code) {
  return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) || (code >= 3000 && code <= 4999);
}

export function acceptKey(key) {
  return createHash('sha1').update(key + GUID).digest('base64');
}

// Encodes one unmasked frame header for a payload of `len` bytes.
export function frameHeader(opcode, len, fin = true) {
  const b0 = (fin ? 0x80 : 0) | opcode;
  if (len < 126) return Buffer.from([b0, len]);
  if (len < 65536) {
    const h = Buffer.alloc(4);
    h[0] = b0;
    h[1] = 126;
    h.writeUInt16BE(len, 2);
    return h;
  }
  const h = Buffer.alloc(10);
  h[0] = b0;
  h[1] = 127;
  h.writeBigUInt64BE(BigInt(len), 2);
  return h;
}

// Incremental decoder for client→server frames. Returns frames as
// { fin, opcode, payload } or throws { code, reason } on a violation.
export class FrameDecoder {
  constructor({ requireMask = true, maxPayload = kMaxClientMessage } = {}) {
    this.buf = Buffer.alloc(0);
    this.requireMask = requireMask;
    this.maxPayload = maxPayload;
  }

  push(chunk) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    const out = [];
    for (;;) {
      const b = this.buf;
      if (b.length < 2) break;
      const fin = (b[0] & 0x80) !== 0;
      if (b[0] & 0x70) throw { code: 1002, reason: 'reserved bits set' };
      const opcode = b[0] & 0x0f;
      const masked = (b[1] & 0x80) !== 0;
      let len = b[1] & 0x7f;
      let off = 2;
      if (len === 126) {
        if (b.length < 4) break;
        len = b.readUInt16BE(2);
        off = 4;
      } else if (len === 127) {
        if (b.length < 10) break;
        const big = b.readBigUInt64BE(2);
        if (big > BigInt(this.maxPayload)) throw { code: 1009, reason: 'message too big' };
        len = Number(big);
        off = 10;
      }
      if (opcode >= 8 && (len > 125 || !fin)) throw { code: 1002, reason: 'bad control frame' };
      if (![0, 1, 2, 8, 9, 10].includes(opcode)) throw { code: 1002, reason: 'unknown opcode' };
      if (this.requireMask && !masked) throw { code: 1002, reason: 'client frame not masked' };
      if (len > this.maxPayload) throw { code: 1009, reason: 'message too big' };
      const need = off + (masked ? 4 : 0) + len;
      if (b.length < need) break;
      let payload = Buffer.from(b.subarray(off + (masked ? 4 : 0), need));
      if (masked) {
        const m = b.subarray(off, off + 4);
        for (let i = 0; i < payload.length; i++) payload[i] ^= m[i & 3];
      }
      out.push({ fin, opcode, payload });
      this.buf = b.subarray(need);
    }
    if (this.buf.length === 0) this.buf = Buffer.alloc(0);
    return out;
  }
}

// One accepted connection. Events: 'message' (Buffer, isBinary), 'close'
// (code, reason). `bufferedBytes` is what the kernel has not taken yet: the
// gateway's backpressure signal.
export class WebSocketConnection extends EventEmitter {
  constructor(socket, { maxPayload = kMaxClientMessage } = {}) {
    super();
    this.socket = socket;
    this.open = true;
    this.decoder = new FrameDecoder({ maxPayload });
    this.fragments = null; // { opcode, parts, size } of a fragmented message
    this.maxPayload = maxPayload;
    socket.setNoDelay(true);
    socket.on('data', (chunk) => this.onData(chunk));
    socket.on('close', () => this.finish(1006, 'connection lost'));
    // HTTP server sockets allow half-open connections: a peer that sends FIN
    // without a close frame would otherwise linger until a write fails.
    socket.on('end', () => this.close(1000, 'peer closed'));
    socket.on('error', () => this.finish(1006, 'socket error'));
    socket.on('drain', () => this.emit('drain'));
  }

  get bufferedBytes() {
    return this.socket.writableLength;
  }

  onData(chunk) {
    let frames;
    try {
      frames = this.decoder.push(chunk);
    } catch (e) {
      this.close(e.code ?? 1002, e.reason ?? 'protocol error');
      return;
    }
    for (const f of frames) {
      if (!this.open) return;
      this.onFrame(f);
    }
  }

  onFrame({ fin, opcode, payload }) {
    switch (opcode) {
      case Opcode.kPing:
        this.sendFrame(Opcode.kPong, payload);
        return;
      case Opcode.kPong:
        return;
      case Opcode.kClose: {
        // Echo a valid code (RFC 6455 §7.4); a malformed close is a protocol error.
        if (payload.length === 0) {
          this.close(1000);
          return;
        }
        const code = payload.length >= 2 ? payload.readUInt16BE(0) : 0;
        this.close(isValidCloseCode(code) ? code : 1002);
        return;
      }
      case Opcode.kCont: {
        if (!this.fragments) {
          this.close(1002, 'continuation without a message');
          return;
        }
        this.fragments.parts.push(payload);
        this.fragments.size += payload.length;
        if (this.fragments.size > this.maxPayload) {
          this.close(1009, 'message too big');
          return;
        }
        if (fin) {
          const { opcode: op, parts } = this.fragments;
          this.fragments = null;
          this.deliver(op, Buffer.concat(parts));
        }
        return;
      }
      default:
        if (this.fragments) {
          this.close(1002, 'new message inside a fragmented one');
          return;
        }
        if (!fin) {
          this.fragments = { opcode, parts: [payload], size: payload.length };
          return;
        }
        this.deliver(opcode, payload);
    }
  }

  deliver(opcode, payload) {
    if (opcode === Opcode.kText) {
      const text = new TextDecoder('utf-8', { fatal: true });
      try {
        this.emit('message', Buffer.from(text.decode(payload)), false);
      } catch {
        this.close(1007, 'invalid UTF-8');
      }
      return;
    }
    this.emit('message', payload, true);
  }

  sendFrame(opcode, payload) {
    if (!this.open) return false;
    this.socket.write(frameHeader(opcode, payload.length));
    return this.socket.write(payload);
  }

  // Sends a binary message made of several buffers without copying them together.
  sendBinary(parts) {
    if (!this.open) return false;
    let len = 0;
    for (const p of parts) len += p.length;
    this.socket.cork();
    this.socket.write(frameHeader(Opcode.kBinary, len));
    let ok = true;
    for (const p of parts) ok = this.socket.write(p);
    this.socket.uncork();
    return ok;
  }

  sendText(str) {
    return this.sendFrame(Opcode.kText, Buffer.from(str, 'utf8'));
  }

  close(code = 1000, reason = '') {
    if (!this.open) return;
    const r = Buffer.from(reason, 'utf8').subarray(0, 123);
    const p = Buffer.alloc(2 + r.length);
    p.writeUInt16BE(code, 0);
    r.copy(p, 2);
    this.sendFrame(Opcode.kClose, p);
    this.socket.end();
    // Do not wait forever for the peer's close.
    setTimeout(() => this.socket.destroy(), 1000).unref();
    this.finish(code, reason);
  }

  // Drops the connection without the closing handshake (stuck client).
  terminate(code = 1006, reason = 'terminated') {
    this.socket.destroy();
    this.finish(code, reason);
  }

  finish(code, reason) {
    if (!this.open) return;
    this.open = false;
    this.emit('close', code, reason);
  }
}

// Completes the handshake for an HTTP 'upgrade' event. Returns the
// connection, or null after answering 400/426 to a bad request.
export function acceptUpgrade(req, socket, head, opts = {}) {
  const key = req.headers['sec-websocket-key'];
  const upgrade = (req.headers.upgrade || '').toLowerCase();
  const version = req.headers['sec-websocket-version'];
  if (req.method !== 'GET' || upgrade !== 'websocket' || !key || Buffer.from(key, 'base64').length !== 16) {
    socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
    return null;
  }
  if (version !== '13') {
    socket.end('HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\nConnection: close\r\n\r\n');
    return null;
  }
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${acceptKey(key)}\r\n\r\n`,
  );
  const ws = new WebSocketConnection(socket, opts);
  if (head && head.length) ws.onData(head);
  return ws;
}
