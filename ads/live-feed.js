// ---------------------------------------------------------------------------
// Live feed, shared by the 2D map (app.js) and the 3D view (cesium-app.js)
//
// Served by the WebSocket gateway (gateway/), a page keeps a live copy of the
// track table from /gateway/ws and redraws within `render_ms` (default 100 ms,
// ?render_ms=N overrides) of each update, instead of polling /api/aircraft
// every 2 s. The feed:
//   - acknowledges frames (flow control: window 8, an ack every 4 frames), so
//     a page that falls behind gets conflated frames instead of a backlog;
//   - reconnects with backoff; the page's 2 s poll covers the time out of sync;
//   - calls onRender(aircraft) with the whole table, in the shape of
//     /api/aircraft (gateway_protocol.mjs toAircraft), at most once per render
//     interval and on an animation frame.
// Served by the Fall 2025 server.js there is no gateway: the protocol module
// fails to load, onUnavailable() is called and the page keeps polling.
// window.__gateway is the feed object, for tests and latency measurement.
//
//   const feed = startLiveFeed({ onRender, onStatus, onUnavailable });
//   (protocolUrl: where to load gateway_protocol.mjs from; tests only)
//   feed.synced  -> the live table is complete; the page can stop polling
// ---------------------------------------------------------------------------
(function () {
    'use strict';

    function startLiveFeed(opts) {
        const params = new URLSearchParams(location.search);
        const feed = {
            connected: false,
            synced: false, // first full table received
            animate: params.get('animate') === '1',
            renderMs: Math.max(0, Number(params.get('render_ms') ?? opts.renderMs ?? 100)),
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
            backoff: 250,
            available: null, // true once the protocol module loaded, false without a gateway
            renderTimeMs: 0, // last onRender call
            maxRenderTimeMs: 0,
        };
        window.__gateway = feed;
        let proto = null;
        let renderTimer = null;
        let renderScheduled = false;
        let socket = null;
        let reconnectTimer = null;
        let stopped = false;
        // Closes the connection for good (no reconnect).
        feed.stop = () => {
            stopped = true;
            clearTimeout(renderTimer);
            clearTimeout(reconnectTimer);
            if (socket) socket.close();
        };

        function aircraftList() {
            const aircraft = [];
            for (const [icao, u] of feed.table.tracks) {
                const seen = feed.seen.get(icao);
                const a = proto.toAircraft(u, seen ? Math.floor(seen / 1000) : null);
                if (a.lat === null || a.lon === null) continue;
                aircraft.push(a);
            }
            return aircraft;
        }

        function reportStatus() {
            if (!opts.onStatus) return;
            if (!feed.connected) opts.onStatus(false, 'Gateway disconnected');
            else if (!feed.synced) opts.onStatus(false, 'Syncing...');
            else if (feed.flags & proto.FrameFlags.kStale) opts.onStatus(false, 'Recovering feed...');
            else if (feed.flags & proto.FrameFlags.kEnded) opts.onStatus(false, 'Feed ended');
            else opts.onStatus(true, 'Live (WebSocket)');
        }

        function render() {
            renderScheduled = false;
            feed.lastRenderAt = performance.now();
            feed.renders++;
            if (feed.synced) {
                const t0 = performance.now();
                try {
                    opts.onRender(aircraftList(), feed);
                } catch (error) {
                    console.error('Error drawing the live feed:', error);
                }
                feed.renderTimeMs = performance.now() - t0; // the page's own update, not the browser's paint
                feed.maxRenderTimeMs = Math.max(feed.maxRenderTimeMs, feed.renderTimeMs);
            }
            reportStatus();
        }

        function scheduleRender() {
            if (renderScheduled) return;
            renderScheduled = true;
            const wait = Math.max(0, feed.lastRenderAt + feed.renderMs - performance.now());
            clearTimeout(renderTimer);
            renderTimer = setTimeout(() => requestAnimationFrame(render), wait);
        }

        function connect() {
            if (stopped) return;
            const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/gateway/ws`);
            socket = ws;
            ws.binaryType = 'arraybuffer';
            let processed = 0;
            let acked = 0;
            const ack = new Uint8Array(5);
            ack[0] = 0x41; // 'A'
            const ackView = new DataView(ack.buffer);
            ws.onopen = () => {
                ws.send(JSON.stringify({ type: 'hello', window: 8 }));
                feed.connected = true;
                feed.backoff = 250;
                reportStatus();
            };
            ws.onmessage = (ev) => {
                if (typeof ev.data === 'string') {
                    try {
                        feed.status = JSON.parse(ev.data);
                    } catch {
                        // ignore
                    }
                    return;
                }
                // Count every binary frame, even one that fails to decode, or the
                // gateway would wait for its ack forever. An ack every 4 frames keeps
                // a window of 8 from running dry.
                processed++;
                if (processed - acked >= 4) {
                    acked = processed;
                    ackView.setUint32(1, acked, true);
                    ws.send(ack);
                }
                let frame;
                try {
                    frame = proto.decodeFrame(ev.data);
                } catch (error) {
                    console.error('gateway frame', error);
                    return;
                }
                const now = Date.now();
                const touched = feed.table.apply(frame);
                if (touched === null) { // a full table
                    feed.seen.clear();
                    for (const icao of feed.table.tracks.keys()) feed.seen.set(icao, now);
                    feed.synced = true;
                } else {
                    for (const icao of touched) {
                        if (feed.table.tracks.has(icao)) feed.seen.set(icao, now);
                        else feed.seen.delete(icao);
                    }
                }
                feed.frames++;
                feed.asOfSeq = frame.asOfSeq;
                feed.flags = frame.flags;
                feed.lastFrameAt = performance.now();
                scheduleRender();
            };
            ws.onclose = () => {
                feed.connected = false;
                feed.synced = false; // the page's poll takes over until the next full table
                reportStatus();
                feed.reconnects++;
                const backoff = Math.min(5000, feed.backoff);
                feed.backoff = backoff * 2;
                if (!stopped) reconnectTimer = setTimeout(connect, backoff);
            };
        }

        import(opts.protocolUrl ?? '/gateway/gateway_protocol.mjs').then((m) => {
            proto = m;
            feed.table = new m.TrackTable();
            feed.available = true;
            connect();
        }, () => {
            // No gateway (the Fall 2025 server): keep polling.
            feed.available = false;
            if (opts.onUnavailable) opts.onUnavailable();
        });
        return feed;
    }

    window.startLiveFeed = startLiveFeed;
})();
