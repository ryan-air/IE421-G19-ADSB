// Feeds the Fall 2025 map (ads/app.js, unmodified) from the gateway's
// WebSocket instead of 2-second polls of a Postgres-backed API.
//
// The gateway adds this script to ads/index.html after app.js. It:
//   1. keeps a live copy of the track table from /gateway/ws;
//   2. answers the page's own fetch('/api/aircraft...') from that table, so
//      app.js's rendering code runs unchanged;
//   3. calls app.js's fetchAircraftData() whenever frames arrive (at most once
//      per `render_ms`, default 100 ms) instead of waiting for its 2 s timer;
//   4. acknowledges frames (flow control: window 8, an ack every 4 frames), so if the page falls
//      behind the gateway conflates instead of queueing frames in the kernel;
//   5. replaces app.js's 2 s marker animation with an immediate move, so the
//      map shows the latest position, not one that is still easing in.
//      Add ?animate=1 to the URL to keep the original animation.
//
// window.__gateway exposes counters for tests and latency measurement.

(() => {
  'use strict';
  const params = new URLSearchParams(location.search);
  const renderMs = Math.max(0, Number(params.get('render_ms') ?? 100));
  const animate = params.get('animate') === '1';

  const state = {
    connected: false,
    synced: false, // first snapshot received
    frames: 0,
    renders: 0,
    reconnects: 0,
    asOfSeq: 0,
    flags: 0,
    lastFrameAt: 0,
    lastRenderAt: 0,
    seen: new Map(), // icao -> wall ms of the last update
    table: null,
    status: null,
  };
  window.__gateway = state;

  let proto = null;
  let renderTimer = null;
  let renderScheduled = false;

  if (!animate && typeof window.smoothMoveMarker === 'function') {
    // Same signature as app.js; jumps instead of easing over `duration`.
    window.smoothMoveMarker = (marker, lat, lon, heading) => {
      marker.setLatLng([lat, lon]);
      if (marker._icon) {
        const icon = marker._icon.querySelector('.aircraft-icon');
        if (icon) icon.style.transform = `rotate(${heading}deg)`;
      }
      marker._heading = heading;
    };
  }

  const aircraftJson = () => {
    const aircraft = [];
    for (const [icao, u] of state.table.tracks) {
      const s = state.seen.get(icao);
      const a = proto.toAircraft(u, s ? Math.floor(s / 1000) : null);
      if (a.lat === null || a.lon === null) continue;
      aircraft.push(a);
    }
    return { now: Date.now(), aircraft, timeWindow: 0, source: 'gateway-ws', as_of_seq: state.asOfSeq };
  };

  const realFetch = window.fetch.bind(window);
  window.fetch = (input, init) => {
    const url = typeof input === 'string' ? input : input && input.url;
    if (state.synced && typeof url === 'string') {
      const path = new URL(url, location.href).pathname;
      if (path === '/api/aircraft') {
        return Promise.resolve(
          new Response(JSON.stringify(aircraftJson()), { headers: { 'content-type': 'application/json' } }),
        );
      }
    }
    return realFetch(input, init);
  };

  const setStatus = () => {
    if (typeof window.updateStatus !== 'function') return;
    if (!state.connected) window.updateStatus(false, 'Gateway disconnected');
    else if (!state.synced) window.updateStatus(false, 'Syncing...');
    else if (state.flags & proto.FrameFlags.kStale) window.updateStatus(false, 'Recovering feed...');
    else if (state.flags & proto.FrameFlags.kEnded) window.updateStatus(false, 'Feed ended');
    else window.updateStatus(true, 'Live (WebSocket)');
  };

  const render = () => {
    renderScheduled = false;
    state.lastRenderAt = performance.now();
    state.renders++;
    if (typeof window.fetchAircraftData === 'function') {
      Promise.resolve(window.fetchAircraftData()).then(setStatus, setStatus);
    }
  };

  const scheduleRender = () => {
    if (renderScheduled) return;
    renderScheduled = true;
    const wait = Math.max(0, state.lastRenderAt + renderMs - performance.now());
    clearTimeout(renderTimer);
    renderTimer = setTimeout(() => requestAnimationFrame(render), wait);
  };

  const connect = () => {
    const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/gateway/ws`);
    ws.binaryType = 'arraybuffer';
    let processed = 0;
    let acked = 0;
    const ack = new Uint8Array(5);
    ack[0] = 0x41; // 'A'
    const ackView = new DataView(ack.buffer);
    ws.onopen = () => {
      ws.send(JSON.stringify({ type: 'hello', window: 8 }));
      state.connected = true;
      state.backoff = 250;
      setStatus();
    };
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') {
        try {
          state.status = JSON.parse(ev.data);
        } catch {
          // ignore
        }
        return;
      }
      // Count every binary frame, even one that fails to decode, or the
      // gateway would wait for it forever. Processing below is synchronous.
      // An ack every 4 frames keeps a window of 8 from ever running dry.
      processed++;
      if (processed - acked >= 4) {
        acked = processed;
        ackView.setUint32(1, acked, true);
        ws.send(ack);
      }
      let frame;
      try {
        frame = proto.decodeFrame(ev.data);
      } catch (e) {
        console.error('gateway frame', e);
        return;
      }
      const now = Date.now();
      const touched = state.table.apply(frame);
      if (touched === null) {
        state.seen.clear();
        for (const icao of state.table.tracks.keys()) state.seen.set(icao, now);
        state.synced = true;
      } else {
        for (const icao of touched) {
          if (state.table.tracks.has(icao)) state.seen.set(icao, now);
          else state.seen.delete(icao);
        }
      }
      state.frames++;
      state.asOfSeq = frame.asOfSeq;
      state.flags = frame.flags;
      state.lastFrameAt = performance.now();
      scheduleRender();
    };
    ws.onclose = () => {
      state.connected = false;
      state.synced = false;
      setStatus();
      state.reconnects++;
      const backoff = Math.min(5000, state.backoff || 250);
      state.backoff = backoff * 2;
      setTimeout(connect, backoff);
    };
  };

  import('/gateway/gateway_protocol.mjs').then((m) => {
    proto = m;
    state.table = new m.TrackTable();
    connect();
  });
})();
