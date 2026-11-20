// Fan-out and flow-control rules driven directly, with stub connections, so
// timing-dependent cases (a socket that stays full, a feed that goes quiet)
// are deterministic. Regression tests for review findings.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { FrameFlags, decodeFrame } from '../public/gateway_protocol.mjs';
import { Gateway } from '../src/gateway.mjs';
import { sleep, waitFor } from './net_fixture.mjs';

function stubClient(overrides = {}) {
  const frames = [];
  const ws = {
    open: true,
    bufferedBytes: 0,
    sendBinary: (parts) => frames.push(decodeFrame(Buffer.concat(parts))),
    sendText: () => {},
    close: () => {},
    terminate() {
      this.open = false;
    },
  };
  const c = { ws, dirty: new Set(), needSnapshot: true, stuckSince: 0, sentFlags: -1, sentSeq: -1, window: 0, sentFrames: 0, ackedFrames: 0, ...overrides };
  return { c, frames };
}

const ack = (n) => {
  const b = Buffer.alloc(5);
  b[0] = 0x41;
  b.writeUInt32LE(n, 1);
  return b;
};

function gateway(opts = {}) {
  // Not started: no sockets. flush() and onClientMessage() work on their own.
  return new Gateway({ clockHelper: null, ...opts });
}

test('a client skipped once is retried even if no more feed input arrives', async () => {
  const g = gateway();
  const { c, frames } = stubClient();
  c.ws.bufferedBytes = 100; // kernel full; no 'drain' will come
  g.clients.add(c);
  g.flush();
  assert.equal(frames.length, 0);
  c.ws.bufferedBytes = 0; // the kernel took it, silently
  // Wait for the outcome, not a fixed time: after an event-loop stall the
  // test's own timer can fire in the same pass as the retry timer, before
  // the flush that timer schedules has run.
  await waitFor(() => frames.length > 0, 2000, 'the retry timer to resend');
  assert.equal(frames.length, 1, 'retried by the timer');
  assert.equal(c.needSnapshot, false);
  clearTimeout(g.retryTimer);
});

test('an ack re-sends a pending flag change (ENDED) even with nothing dirty', async () => {
  const g = gateway();
  const { c, frames } = stubClient({ window: 1 });
  g.clients.add(c);
  g.flush(); // snapshot frame: credit used up
  assert.equal(frames.length, 1);
  g.core.eos = true; // end of session seen, everything applied
  g.core.eosSeq = 1;
  assert.ok(g.core.ended);
  g.flush(); // held: no credit
  assert.equal(frames.length, 1);
  g.onClientMessage(c, ack(1), true);
  assert.equal(g.flushScheduled, true, 'the ack itself schedules the flush (not just the retry timer)');
  await sleep(50);
  assert.equal(frames.length, 2);
  assert.equal(frames[1].flags & FrameFlags.kEnded, FrameFlags.kEnded);
  assert.equal(frames[1].messages.length, 0);
  clearTimeout(g.retryTimer);
});

test('a client out of credit is dropped after stuckMs even on a quiet feed', async () => {
  const g = gateway({ stuckMs: 50 });
  const { c, frames } = stubClient({ window: 1 });
  g.clients.add(c);
  g.flush();
  c.dirty.add(1); // a change it will never get credit for
  g.flush();
  await waitFor(() => !c.ws.open, 2000, 'the stuck client to be dropped'); // see the first test
  assert.equal(frames.length, 1);
  assert.equal(c.ws.open, false, 'terminated as stuck');
  assert.equal(g.counters.clients_dropped_stuck, 1);
  clearTimeout(g.retryTimer);
});
