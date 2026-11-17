// FeedClient: SubscriberCore with real sockets, the counterpart of the C++
// SubscriberClient. The feed arrives on a UDP socket (multicast group or
// unicast address), retransmit requests go out and replies come back on a
// second, unicast UDP socket, and snapshots are fetched over TCP.
//
// Unlike the C++ client, the snapshot fetch is asynchronous: the feed keeps
// being processed (and buffered, in snapshot mode) while it is in flight,
// which is what the core expects anyway.

import dgram from 'node:dgram';
import net from 'node:net';

import { hrNs } from './clock.mjs';
import { SnapshotParser, Source, SubscriberCore, encodeRetransmitRequest } from './feed.mjs';

export function parseEndpoint(text) {
  const i = text.lastIndexOf(':');
  if (i <= 0) throw new Error(`expected HOST:PORT, got "${text}"`);
  const port = Number(text.slice(i + 1));
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error(`bad port in "${text}"`);
  return { host: text.slice(0, i), port };
}

export function isMulticast(host) {
  const m = /^(\d+)\.\d+\.\d+\.\d+$/.exec(host);
  return m !== null && Number(m[1]) >= 224 && Number(m[1]) <= 239;
}

// Deterministic loss injection on received feed datagrams (same knobs as the
// C++ LossInjector, but its own generator: the dropped packets differ).
export class LossInjector {
  constructor({ dropRate = 0, dropBurst = 1, seed = 1, onlyFirst = 0 } = {}) {
    this.rate = dropRate;
    this.burst = Math.max(1, dropBurst);
    this.state = (seed >>> 0) || 1;
    this.onlyFirst = onlyFirst;
    this.seen = 0;
    this.left = 0;
    this.dropped = 0;
  }

  next() {
    // xorshift32
    let x = this.state;
    x ^= x << 13;
    x >>>= 0;
    x ^= x >>> 17;
    x ^= x << 5;
    this.state = x >>> 0;
    return this.state / 2 ** 32;
  }

  drop() {
    this.seen++;
    if (this.rate <= 0) return false;
    if (this.onlyFirst > 0 && this.seen > this.onlyFirst) return false;
    if (this.left > 0) {
      this.left--;
      this.dropped++;
      return true;
    }
    if (this.next() < this.rate) {
      this.left = this.burst - 1;
      this.dropped++;
      return true;
    }
    return false;
  }
}

export function fetchSnapshot({ host, port }, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const parser = new SnapshotParser();
    const sock = net.connect({ host, port });
    let settled = false;
    const finish = (err) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      sock.destroy();
      if (err) reject(err);
      else resolve(parser.data);
    };
    const timer = setTimeout(() => finish(new Error('snapshot: timed out')), timeoutMs);
    sock.on('data', (chunk) => {
      const st = parser.feed(chunk);
      if (st === 'error') finish(new Error(`snapshot: ${parser.error}`));
      else if (st === 'done') finish(null); // complete: no need to wait for the close
    });
    sock.on('end', () =>
      finish(parser.state === 'done' ? null : new Error('snapshot: connection closed before SnapshotEnd')),
    );
    sock.on('error', (e) => finish(new Error(`snapshot: ${e.message}`)));
  });
}

export class FeedClient {
  // opts: { feed, interfaceHost, retransmit, snapshot, rcvbuf, snapshotTimeoutMs,
  //         loss, core (SubscriberCore config), handler, pollMs }
  constructor(opts) {
    this.opts = {
      feed: { host: '239.19.0.1', port: 41900 },
      interfaceHost: undefined,
      retransmit: { host: '127.0.0.1', port: 41901 },
      snapshot: { host: '127.0.0.1', port: 41902 },
      rcvbuf: 8 << 20,
      snapshotTimeoutMs: 3000,
      pollMs: 2,
      ...opts,
    };
    this.loss = new LossInjector(this.opts.loss ?? {});
    this.core = new SubscriberCore(
      this.opts.core ?? {},
      { sendRequest: (s, seq, count) => this.sendRequest(s, seq, count) },
      this.opts.handler ?? null,
    );
    this.snapshotInFlight = false;
    this.lastSnapshotError = '';
    this.onEnded = null;
    this.onInput = null; // called after every datagram and timer tick
  }

  async open() {
    const { feed, interfaceHost } = this.opts;
    const mcast = isMulticast(feed.host);
    this.feedSock = dgram.createSocket({ type: 'udp4', reuseAddr: mcast });
    this.reqSock = dgram.createSocket({ type: 'udp4' });
    // Binding a multicast receiver to the group address (as the C++ client
    // does) keeps unrelated traffic to the same port out on Linux.
    await new Promise((res, rej) => {
      this.feedSock.once('error', rej);
      this.feedSock.bind({ port: feed.port, address: feed.host }, () => {
        this.feedSock.off('error', rej);
        res();
      });
    });
    if (mcast) this.feedSock.addMembership(feed.host, interfaceHost);
    await new Promise((res, rej) => {
      this.reqSock.once('error', rej);
      this.reqSock.bind({ port: 0, address: '0.0.0.0' }, () => {
        this.reqSock.off('error', rej);
        res();
      });
    });
    for (const s of [this.feedSock, this.reqSock]) {
      try {
        s.setRecvBufferSize(this.opts.rcvbuf);
      } catch {
        // The kernel cap (net.core.rmem_max) is lower: keep its default.
      }
    }
    this.feedSock.on('message', (msg) => {
      if (this.loss.drop()) return;
      this.core.onDatagram(msg, Source.kFeed, hrNs());
      this.afterInput();
    });
    this.reqSock.on('message', (msg, rinfo) => {
      // Only the retransmit server answers here; match its port (a server on
      // 0.0.0.0 may answer from another local address).
      if (rinfo.port !== this.opts.retransmit.port) return;
      this.core.onDatagram(msg, Source.kRetransmit, hrNs());
      this.afterInput();
    });
    this.timer = setInterval(() => {
      this.core.poll(hrNs());
      this.afterInput();
    }, this.opts.pollMs);
    return this;
  }

  feedPort() {
    return this.feedSock.address().port;
  }

  sendRequest(session, seq, count) {
    this.reqSock.send(encodeRetransmitRequest(session, seq, count), this.opts.retransmit.port, this.opts.retransmit.host);
  }

  afterInput() {
    if (this.closed) return;
    this.onInput?.();
    if (!this.snapshotInFlight && this.core.takeSnapshotRequest(hrNs())) this.startSnapshot();
    if (this.core.ended && this.onEnded) {
      const cb = this.onEnded;
      this.onEnded = null;
      cb();
    }
  }

  startSnapshot() {
    this.snapshotInFlight = true;
    fetchSnapshot(this.opts.snapshot, this.opts.snapshotTimeoutMs)
      .then((snap) => {
        if (!this.closed) this.core.onSnapshot(snap, hrNs());
      })
      .catch((e) => {
        if (this.closed) return;
        this.lastSnapshotError = e.message;
        this.core.onSnapshotFailed(hrNs());
      })
      .finally(() => {
        this.snapshotInFlight = false;
        this.afterInput();
      });
  }

  // Resolves once the session has ended and everything before the end is applied.
  untilEnded() {
    if (this.core.ended) return Promise.resolve();
    return new Promise((res) => {
      this.onEnded = res;
    });
  }

  close() {
    // An in-flight snapshot fetch finishes on its own (within its timeout);
    // its result is then ignored and no further fetch is started.
    this.closed = true;
    clearInterval(this.timer);
    this.feedSock?.close();
    this.reqSock?.close();
  }
}
