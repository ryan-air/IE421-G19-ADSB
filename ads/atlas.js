// ---------------------------------------------------------------------------
// Atlas: the presentation layer of the 2D map.
//
// app.js owns the data (WebSocket feed or polling), the markers, trails,
// predictions and collision detection. This file draws everything around
// them and never changes what they compute:
//   - data-block placement around each aircraft symbol (no overlaps);
//   - selection: highlight, recorded track, projection, floating annotation;
//   - the timeline: a recorder of every table the page is shown (seeded with
//     the gateway's recent trail points), scrubbing and playback, events;
//   - the edge rail and its sheets (layers, index, analysis);
//   - masthead, instrument readouts, altitude tape, scale bar.
// Nothing here invents data: playback shows recorded fixes (linear between
// fixes at most 10 s apart), the future shows dead reckoning, labelled as a
// projection, and readouts the source cannot provide say so.
// ---------------------------------------------------------------------------
const atlas = (() => {
    const RECEIVER = [41.88, -87.63]; // the receiver; also the map's default centre (app.js)
    const HISTORY_MS = 15 * 60 * 1000;   // timeline: past
    const FUTURE_MS = 5 * 60 * 1000;     // timeline: projection
    const LOST_AFTER_MS = 60 * 1000;     // a recorded aircraft vanishes after this without fixes
    const INTERP_MAX_MS = 10 * 1000;     // interpolate only between close fixes
    const KT = 0.514444;                 // m/s per knot
    const NM = 1852;
    const EMERGENCY = { '7500': 'Unlawful interference', '7600': 'Radio failure', '7700': 'General emergency' };

    let map = null;
    const $ = (id) => document.getElementById(id);
    const fmt = (n) => (n === null || n === undefined || Number.isNaN(n) ? '—' : Math.round(n).toLocaleString('en-US'));
    const pad2 = (n) => String(n).padStart(2, '0');
    const clock = (t) => { const d = new Date(t); return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`; };
    const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

    function dm(value, pos, neg, degDigits) {
        const a = Math.abs(value);
        const d = Math.floor(a);
        const m = (a - d) * 60;
        return `${String(d).padStart(degDigits, '0')}°${m.toFixed(2).padStart(5, '0')}′${value >= 0 ? pos : neg}`;
    }
    const coords = (lat, lon) => `${dm(lat, 'N', 'S', 2)}  ${dm(lon, 'E', 'W', 3)}`;

    function signedDuration(ms) {
        const s = Math.round(Math.abs(ms) / 1000);
        const sign = ms < 0 ? '−' : '+';
        return `${sign}${pad2(Math.floor(s / 60))}:${pad2(s % 60)}`;
    }

    // -----------------------------------------------------------------------
    // Recorder: what the page has been shown, per aircraft
    // -----------------------------------------------------------------------
    const rec = {
        tracks: new Map(),   // hex -> {fixes: [{t,lat,lon,alt,trk,spd,vr,gnd,sq}], flight, first}
        counts: [],          // [{t, n}]
        events: [],          // [{t, kind, hex, text}]
        since: null,         // earliest recorded time
        lastHexes: null,
        lastCountAt: 0,
        lastPruneAt: 0,
        squawks: new Map(),
    };

    function sampleInterval(n) {
        // 2 s per aircraft, longer with large tables (bounded memory)
        return 2000 * Math.max(1, n / 500);
    }

    function addFix(hex, a, t, flight) {
        let tr = rec.tracks.get(hex);
        if (!tr) rec.tracks.set(hex, (tr = { fixes: [], flight: null, first: t }));
        if (flight) tr.flight = flight;
        const f = tr.fixes;
        if (f.length && t <= f[f.length - 1].t) return;
        f.push({ t, lat: a.lat, lon: a.lon, alt: a.altitude ?? null, trk: a.track ?? null,
                 spd: a.speed ?? null, vr: a.vertical_rate ?? null, gnd: !!a.on_ground, sq: a.squawk ?? null });
        if (rec.since === null || t < rec.since) rec.since = t;
    }

    function event(t, kind, hex, text) {
        rec.events.push({ t, kind, hex, text });
        if (rec.events.length > 5000) rec.events.splice(0, rec.events.length - 5000);
    }

    function record(aircraft, now) {
        const interval = sampleInterval(aircraft.length);
        const hexes = new Set();
        for (const a of aircraft) {
            if (a.lat === null || a.lon === null || a.lat === undefined) continue;
            hexes.add(a.hex);
            const tr = rec.tracks.get(a.hex);
            const last = tr?.fixes[tr.fixes.length - 1];
            if (!last || now - last.t >= interval || hexes.size === 0) addFix(a.hex, a, now, a.flight);
            else if (a.flight && tr) tr.flight = a.flight;

            const sq = a.squawk ?? null;
            const prevSq = rec.squawks.get(a.hex);
            if (sq !== prevSq) {
                rec.squawks.set(a.hex, sq);
                if (EMERGENCY[sq]) event(now, 'alert', a.hex, `${a.flight || a.hex} squawks ${sq} · ${EMERGENCY[sq]}`);
            }
        }
        if (rec.lastHexes === null) {
            event(now, 'feed', null, `Initial picture · ${hexes.size} aircraft`);
        } else {
            let acquired = 0;
            for (const h of hexes) if (!rec.lastHexes.has(h)) { acquired++; event(now, 'acquired', h, `Acquired ${rec.tracks.get(h)?.flight || h}`); }
            for (const h of rec.lastHexes) if (!hexes.has(h)) event(now, 'lost', h, `Lost ${rec.tracks.get(h)?.flight || h}`);
            void acquired;
        }
        rec.lastHexes = hexes;

        if (now - rec.lastCountAt >= 2000) {
            rec.counts.push({ t: now, n: hexes.size });
            rec.lastCountAt = now;
        }
        if (now - rec.lastPruneAt >= 10000) prune(now);
        live.lastData = now;
        live.tracked = hexes.size;
    }

    function prune(now) {
        rec.lastPruneAt = now;
        const cutoff = now - HISTORY_MS - 60000;
        for (const [hex, tr] of rec.tracks) {
            let i = 0;
            while (i < tr.fixes.length && tr.fixes[i].t < cutoff) i++;
            if (i) tr.fixes.splice(0, i);
            if (!tr.fixes.length && hex !== sel.hex) rec.tracks.delete(hex);
        }
        while (rec.counts.length && rec.counts[0].t < cutoff) rec.counts.shift();
        while (rec.events.length && rec.events[0].t < cutoff) rec.events.shift();
        if (rec.since !== null && rec.since < cutoff) rec.since = cutoff;
    }

    // Seed the recorder with the gateway's recent trail points (about the
    // last 200 fixes per aircraft), so the timeline has a past at once.
    async function seedFromGateway() {
        try {
            const r = await fetch('/api/aircraft/enriched');
            if (!r.ok) return;
            const data = await r.json();
            let n = 0;
            for (const a of data.aircraft || []) {
                const bySecond = new Map();
                for (const p of a.trail || []) if (p.seen) bySecond.set(p.seen, p);
                for (const [sec, p] of [...bySecond.entries()].sort((x, y) => x[0] - y[0])) {
                    addFix(a.hex, { lat: p.lat, lon: p.lon, altitude: p.altitude, track: p.track, speed: p.speed,
                                    vertical_rate: null, on_ground: false, squawk: a.squawk }, sec * 1000, a.flight);
                    n++;
                }
            }
            if (n) {
                rec.seeded = n;
                // aircraft count over the seeded period, from those fixes
                const end = rec.counts.length ? rec.counts[0].t : Date.now();
                const seededCounts = [];
                for (let t = rec.since; t < end; t += 5000) {
                    let c = 0;
                    for (const tr of rec.tracks.values()) if (fixAt(tr, t)) c++;
                    seededCounts.push({ t, n: c });
                }
                rec.counts.unshift(...seededCounts);
                rec.seededUntil = end;
                timeline.draw();
            }
        } catch { /* Fall 2025 server: no enriched trails; record from now */ }
    }

    // Fix of one aircraft at time t (null if not tracked then)
    function fixAt(tr, t) {
        const f = tr.fixes;
        if (!f.length || t < f[0].t) return null;
        let lo = 0, hi = f.length - 1;
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1;
            if (f[mid].t <= t) lo = mid; else hi = mid - 1;
        }
        const a = f[lo];
        if (t - a.t > LOST_AFTER_MS) return null;
        const b = f[lo + 1];
        if (!b || b.t - a.t > INTERP_MAX_MS) return a;
        const k = (t - a.t) / (b.t - a.t);
        let dtrk = (b.trk ?? 0) - (a.trk ?? 0);
        if (dtrk > 180) dtrk -= 360;
        if (dtrk < -180) dtrk += 360;
        return { ...a, t, lat: a.lat + (b.lat - a.lat) * k, lon: a.lon + (b.lon - a.lon) * k,
                 alt: a.alt !== null && b.alt !== null ? a.alt + (b.alt - a.alt) * k : a.alt,
                 trk: a.trk !== null ? (a.trk + dtrk * k + 360) % 360 : null };
    }

    const toAircraft = (hex, tr, f) => ({
        hex, flight: tr.flight, lat: f.lat, lon: f.lon, altitude: f.alt === null ? null : Math.round(f.alt),
        speed: f.spd, track: f.trk === null ? null : Math.round(f.trk), vertical_rate: f.vr,
        squawk: f.sq, on_ground: f.gnd,
    });

    // Dead reckoning: constant track, speed and altitude
    function project(a, dtMs) {
        if (!a.speed || a.track === null || a.track === undefined || a.on_ground) return { ...a };
        const d = a.speed * KT * dtMs / 1000;
        const trk = a.track * Math.PI / 180;
        const lat = a.lat + (d * Math.cos(trk)) / 111320;
        const lon = a.lon + (d * Math.sin(trk)) / (111320 * Math.cos(a.lat * Math.PI / 180));
        return { ...a, lat, lon };
    }

    function frameAt(t) {
        const now = Date.now();
        if (t > now) return currentAircraftData.map((a) => project(a, t - now));
        const out = [];
        for (const [hex, tr] of rec.tracks) {
            const f = fixAt(tr, t);
            if (f) out.push(toAircraft(hex, tr, f));
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // Live state for readouts
    // -----------------------------------------------------------------------
    const live = {
        connected: false, statusText: 'Connecting', lastData: 0, tracked: 0,
        stats: null, statsPrev: null, rate: null, statsAvailable: null,
        collisions: null, collisionNote: null,
    };

    // -----------------------------------------------------------------------
    // Data-block placement
    // -----------------------------------------------------------------------
    // Candidate offsets of the block's anchor corner relative to the symbol,
    // clockwise from upper right. The leader joins the symbol to the block.
    const CANDIDATES = ['ne', 'se', 'nw', 'sw', 'e', 'w', 'n', 's'];
    const GAP = 13;
    const BLOCK_H = 25;
    let layoutTimer = null;
    let lastLayout = 0;
    let labelsOn = true;

    function blockRect(pos, x, y, w, h) {
        switch (pos) {
            case 'ne': return [x + GAP, y - GAP - h];
            case 'se': return [x + GAP, y + GAP];
            case 'nw': return [x - GAP - w, y - GAP - h];
            case 'sw': return [x - GAP - w, y + GAP];
            case 'e': return [x + GAP + 4, y - h / 2];
            case 'w': return [x - GAP - 4 - w, y - h / 2];
            case 'n': return [x - w / 2, y - GAP - 6 - h];
            default: return [x - w / 2, y + GAP + 6];
        }
    }

    function scheduleLayout(now = false) {
        if (now) { clearTimeout(layoutTimer); layoutTimer = null; layout(); return; }
        if (layoutTimer) return;
        const wait = Math.max(0, 140 - (performance.now() - lastLayout));
        layoutTimer = setTimeout(() => { layoutTimer = null; layout(); }, wait);
    }

    function layout() {
        lastLayout = performance.now();
        if (!map) return;
        const size = map.getSize();
        const cell = 48;
        const grid = new Map();
        const key = (cx, cy) => cx * 100003 + cy;
        const add = (r, id) => {
            for (let cx = Math.floor(r[0] / cell); cx <= Math.floor(r[2] / cell); cx++)
                for (let cy = Math.floor(r[1] / cell); cy <= Math.floor(r[3] / cell); cy++) {
                    const k = key(cx, cy);
                    let b = grid.get(k);
                    if (!b) grid.set(k, (b = []));
                    b.push([r, id]);
                }
        };
        const hits = (r, id) => {
            for (let cx = Math.floor(r[0] / cell); cx <= Math.floor(r[2] / cell); cx++)
                for (let cy = Math.floor(r[1] / cell); cy <= Math.floor(r[3] / cell); cy++) {
                    const b = grid.get(key(cx, cy));
                    if (!b) continue;
                    for (const [o, oid] of b)
                        if (oid !== id && r[0] < o[2] && r[2] > o[0] && r[1] < o[3] && r[3] > o[1]) return true;
                }
            return false;
        };

        // reserved: readouts, masthead, tape, timeline, annotation
        for (const el of document.querySelectorAll('[data-reserve]')) {
            if (el.hidden || el.getClientRects().length === 0) continue;
            const r = el.getBoundingClientRect();
            const m = map.getContainer().getBoundingClientRect();
            if (r.width && r.height) add([r.left - m.left - 4, r.top - m.top - 4, r.right - m.left + 4, r.bottom - m.top + 4], -1);
        }

        const items = [];
        for (const [id, m] of aircraftMarkers) {
            if (!m._icon) continue;
            const p = map.latLngToContainerPoint(m.getLatLng());
            if (p.x < -40 || p.y < -40 || p.x > size.x + 40 || p.y > size.y + 40) {
                hideBlock(m);
                continue;
            }
            items.push({ id, m, x: p.x, y: p.y });
            add([p.x - 7, p.y - 7, p.x + 7, p.y + 7], id);
        }
        const rank = (it) => (it.id === sel.hex ? 0 : it.id === hov.hex ? 1 : EMERGENCY[rec.squawks.get(it.id)] ? 2 : it.m._lpos ? 3 : 4);
        items.sort((a, b) => rank(a) - rank(b));

        let shown = 0;
        for (const it of items) {
            const { m } = it;
            const forced = it.id === sel.hex || it.id === hov.hex;
            if (!labelsOn && !forced) { hideBlock(m); continue; }
            const w = blockWidth(m);
            const order = m._lpos ? [m._lpos, ...CANDIDATES.filter((c) => c !== m._lpos)] : CANDIDATES;
            let placed = null;
            for (const pos of order) {
                const [bx, by] = blockRect(pos, it.x, it.y, w, BLOCK_H);
                const r = [bx, by, bx + w, by + BLOCK_H];
                if (bx < 4 || by < 4 || bx + w > size.x - 4 || by + BLOCK_H > size.y - 4) continue;
                if (hits(r, it.id)) continue;
                placed = { pos, bx, by, r };
                break;
            }
            if (!placed && forced) {
                const [bx, by] = blockRect('ne', it.x, it.y, w, BLOCK_H);
                placed = { pos: 'ne', bx, by, r: [bx, by, bx + w, by + BLOCK_H] };
            }
            if (!placed) { hideBlock(m); continue; }
            add(placed.r, it.id);
            showBlock(m, placed.pos, placed.bx - it.x, placed.by - it.y, w);
            shown++;
        }
        live.labelled = shown;
    }

    function blockWidth(m) {
        const el = m._icon.querySelector('.ac-label');
        const text = (m._identifier || '').length;
        if (m._lw && m._lwText === text) return m._lw;
        const w = el && el.offsetWidth ? el.offsetWidth : 8 + Math.max(text, 7) * 6.6;
        m._lw = w;
        m._lwText = text;
        return w;
    }

    function hideBlock(m) {
        if (m._lpos === null) return;
        const root = m._icon?.querySelector('.ac');
        if (root) root.classList.add('no-label');
        m._lpos = null;
    }

    function showBlock(m, pos, dx, dy, w) {
        const root = m._icon.querySelector('.ac');
        if (!root) return;
        root.classList.remove('no-label');
        if (m._lpos === pos && m._ldx === dx && m._ldy === dy) return;
        m._lpos = pos;
        m._ldx = dx;
        m._ldy = dy;
        const label = root.querySelector('.ac-label');
        label.style.transform = `translate(${dx}px, ${dy}px)`;
        label.dataset.pos = pos;
        // leader: from just outside the symbol to the nearest point of the block
        const cx = Math.max(dx, Math.min(0, dx + w));
        const cy = Math.max(dy, Math.min(0, dy + BLOCK_H));
        const len = Math.hypot(cx, cy) || 1;
        const sx = (cx / len) * 8;
        const sy = (cy / len) * 8;
        const line = root.querySelector('.ac-leader line');
        line.setAttribute('x1', sx.toFixed(1));
        line.setAttribute('y1', sy.toFixed(1));
        line.setAttribute('x2', cx.toFixed(1));
        line.setAttribute('y2', cy.toFixed(1));
    }

    // -----------------------------------------------------------------------
    // Hover and selection
    // -----------------------------------------------------------------------
    const hov = { hex: null };
    const sel = { hex: null, layers: null, more: false, follow: false };

    function markerOf(hex) { return aircraftMarkers.get(hex); }

    function setMarkerClass(hex, cls, on) {
        const el = markerOf(hex)?._icon?.querySelector('.ac');
        if (el) el.classList.toggle(cls, on);
        const icon = markerOf(hex)?._icon;
        if (icon) icon.style.zIndex = on ? 100000 : '';
    }

    function hover(hex) {
        if (hov.hex === hex) return;
        if (hov.hex && hov.hex !== sel.hex) setMarkerClass(hov.hex, 'is-hover', false);
        hov.hex = hex;
        if (hex) setMarkerClass(hex, 'is-hover', true);
        for (const row of document.querySelectorAll('.index-row')) row.classList.toggle('is-hover', row.dataset.hex === hex);
        scheduleLayout(true);
    }

    function select(hex, { fly = false } = {}) {
        if (sel.hex && sel.hex !== hex) setMarkerClass(sel.hex, 'is-selected', false);
        sel.hex = hex;
        sel.more = false;
        sel.follow = false;
        if (!hex) {
            clearSelectionLayers();
            $('annotation').hidden = true;
            $('annotation-leader').classList.remove('on');
            document.body.classList.remove('has-selection');
            scheduleLayout(true);
            tape.draw();
            return;
        }
        document.body.classList.add('has-selection');
        setMarkerClass(hex, 'is-selected', true);
        const m = markerOf(hex);
        if (fly && m) map.flyTo(m.getLatLng(), Math.max(map.getZoom(), 9), { duration: 0.6 });
        else if (m && window.innerWidth <= 768) {
            // phones: the annotation is a bottom sheet; keep the aircraft above it
            const p = map.latLngToContainerPoint(m.getLatLng());
            const target = map.getSize().y * 0.28;
            if (p.y > target + 40) map.panBy([0, p.y - target], { duration: 0.4 });
        }
        $('annotation').hidden = false;
        renderAnnotation(true);
        scheduleLayout(true);
        tape.draw();
    }

    function selectedAircraft() {
        if (!sel.hex) return null;
        if (pb.active) return displayed.get(sel.hex) || null;
        return currentAircraftData.find((a) => a.hex === sel.hex) || null;
    }

    function clearSelectionLayers() {
        if (sel.layers) { sel.layers.remove(); sel.layers = null; }
    }

    function drawSelectionLayers(a) {
        clearSelectionLayers();
        if (!a) return;
        const g = L.layerGroup();
        const t = displayTime();
        const projected = t > Date.now();
        const tr = rec.tracks.get(a.hex);
        // where observation ends: the displayed position, or (projected) the
        // live position the projection starts from
        const origin = projected ? (currentAircraftData.find((x) => x.hex === a.hex) || a) : a;
        // recorded track up to the displayed time (never into the projection)
        if (tr) {
            const pts = tr.fixes.filter((f) => f.t <= t).map((f) => [f.lat, f.lon]);
            pts.push([origin.lat, origin.lon]);
            if (pts.length > 1) {
                L.polyline(pts, { pane: 'selectionPane', color: PALETTE.ink, weight: 4, opacity: 0.12, interactive: false }).addTo(g);
                L.polyline(pts, { pane: 'selectionPane', color: PALETTE.ink, weight: 1.4, opacity: 0.9, interactive: false }).addTo(g);
            }
        }
        // projection: five minutes at constant track and speed, minute ticks
        if (a.speed && a.track !== null && a.track !== undefined && !a.on_ground) {
            const ends = [1, 2, 3, 4, 5].map((min) => project(origin, min * 60000));
            L.polyline([[origin.lat, origin.lon], ...ends.map((p) => [p.lat, p.lon])], {
                pane: 'selectionPane', color: PALETTE.ink, weight: 1.1, opacity: 0.55, dashArray: '2 5', interactive: false,
            }).addTo(g);
            ends.forEach((p, i) => {
                L.marker([p.lat, p.lon], {
                    pane: 'selectionPane', interactive: false, keyboard: false,
                    icon: L.divIcon({ className: 'proj-tick', html: `<i></i><span>+${i + 1}′</span>`, iconSize: [0, 0] }),
                }).addTo(g);
            });
        }
        L.marker([a.lat, a.lon], {
            pane: 'selectionPane', interactive: false, keyboard: false,
            icon: L.divIcon({ className: 'sel-ring', html: '<i></i>', iconSize: [0, 0] }),
        }).addTo(g);
        g.addTo(map);
        sel.layers = g;
    }

    function renderAnnotation(rebuild = false) {
        const box = $('annotation');
        if (!sel.hex) return;
        const a = selectedAircraft();
        if (!a) {
            // left the table: keep the annotation, marked as lost
            box.classList.add('is-lost');
            box.querySelector('.ann-kicker-state')?.replaceChildren(document.createTextNode('Signal lost'));
            positionAnnotation();
            return;
        }
        box.classList.remove('is-lost');
        drawSelectionLayers(a);
        const tr = rec.tracks.get(a.hex);
        const t = displayTime();
        const state = pb.active ? (t > Date.now() ? `Projected ${signedDuration(t - Date.now())}` : `Recorded ${clock(t)}`) : 'Live';
        const vr = a.vertical_rate;
        const vrText = vr === null || vr === undefined ? '—' : `${vr > 0 ? '+' : vr < 0 ? '−' : ''}${fmt(Math.abs(vr))}`;
        const seenMs = (() => {
            if (pb.active) return null;
            const s = liveFeed?.seen?.get(Number.parseInt(a.hex, 16));
            if (s) return Date.now() - s;
            return a.seen ? Date.now() - a.seen * 1000 : null;
        })();
        const tracked = tr ? Date.now() - tr.first : null;
        const sq = a.squawk;
        const emergency = EMERGENCY[sq];
        const band = altitudeBand(a);

        if (rebuild || !box.querySelector('.ann-callsign')) {
            box.innerHTML = `
                <button class="ann-close" type="button" aria-label="Close (Esc)">Esc&nbsp;&nbsp;×</button>
                <div class="ann-kicker"><span class="ann-kicker-state"></span><span class="ann-icao"></span></div>
                <div class="ann-callsign"></div>
                <div class="ann-primary">
                    <div class="ann-fig"><span class="ann-big" data-k="alt"></span><span class="ann-unit">ft</span><label>Altitude</label></div>
                    <div class="ann-fig"><span class="ann-big" data-k="spd"></span><span class="ann-unit">kt</span><label>Ground speed</label></div>
                    <div class="ann-fig"><span class="ann-big" data-k="trk"></span><span class="ann-unit">°</span><label>Track</label></div>
                </div>
                <dl class="ann-rows">
                    <dt>Vertical</dt><dd data-k="vr"></dd>
                    <dt>Squawk</dt><dd data-k="sq"></dd>
                    <dt>Position</dt><dd data-k="pos"></dd>
                    <dt data-k="seenLabel">Last message</dt><dd data-k="seen"></dd>
                </dl>
                <button class="ann-more" type="button"></button>
                <div class="ann-detail" hidden>
                    <div class="ann-profile">
                        <label>Altitude, recorded</label>
                        <svg class="ann-spark" data-k="altSpark" viewBox="0 0 240 44" preserveAspectRatio="none"></svg>
                        <label>Ground speed, recorded</label>
                        <svg class="ann-spark" data-k="spdSpark" viewBox="0 0 240 30" preserveAspectRatio="none"></svg>
                    </div>
                    <dl class="ann-rows">
                        <dt>ICAO address</dt><dd data-k="hex"></dd>
                        <dt>Recorded for</dt><dd data-k="tracked"></dd>
                        <dt>Recorded fixes</dt><dd data-k="fixes"></dd>
                    </dl>
                    <div class="ann-actions">
                        <button type="button" data-act="centre">Centre</button>
                        <button type="button" data-act="follow">Follow</button>
                        <button type="button" data-act="trail">Source trail</button>
                    </div>
                </div>`;
            box.querySelector('.ann-close').onclick = () => select(null);
            box.querySelector('.ann-more').onclick = () => { sel.more = !sel.more; renderAnnotation(); };
            box.querySelector('[data-act="centre"]').onclick = () => { const m = markerOf(sel.hex); if (m) map.panTo(m.getLatLng()); };
            box.querySelector('[data-act="follow"]').onclick = () => { sel.follow = !sel.follow; renderAnnotation(); };
            box.querySelector('[data-act="trail"]').onclick = () => { toggleAircraftTrail(sel.hex).then(() => renderAnnotation()); };
        }
        const q = (k) => box.querySelector(`[data-k="${k}"]`);
        box.dataset.band = band;
        box.classList.toggle('is-emergency', !!emergency);
        box.querySelector('.ann-kicker-state').textContent = emergency ? `${sq} · ${emergency}` : state;
        box.querySelector('.ann-icao').textContent = a.hex;
        box.querySelector('.ann-callsign').textContent = a.flight || a.hex;
        q('alt').textContent = a.on_ground ? 'GND' : fmt(a.altitude);
        q('spd').textContent = fmt(a.speed);
        q('trk').textContent = a.track === null || a.track === undefined ? '—' : String(Math.round(a.track)).padStart(3, '0');
        q('vr').innerHTML = vr ? `${vrText} <small>fpm ${vr > 300 ? 'climbing' : vr < -300 ? 'descending' : 'level'}</small>` : `${vrText}${vr === 0 ? ' <small>fpm level</small>' : ''}`;
        q('sq').textContent = sq || '—';
        q('pos').textContent = coords(a.lat, a.lon);
        q('seenLabel').textContent = pb.active ? 'Display time' : 'Last message';
        q('seen').textContent = pb.active ? clock(t) : seenMs === null ? '—' : seenMs < 1000 ? `${Math.max(0, Math.round(seenMs))} ms ago` : `${(seenMs / 1000).toFixed(1)} s ago`;
        const more = box.querySelector('.ann-more');
        more.textContent = sel.more ? 'Less' : 'Profile & detail';
        more.classList.toggle('open', sel.more);
        box.querySelector('.ann-detail').hidden = !sel.more;
        if (sel.more) {
            q('hex').textContent = a.hex;
            q('tracked').textContent = tracked === null ? '—' : formatDuration(Math.round(tracked / 1000));
            q('fixes').textContent = tr ? `${fmt(tr.fixes.length)} since ${clock(tr.fixes[0]?.t ?? tr.first)}` : '—';
            spark(q('altSpark'), tr, 'alt', 44, t);
            spark(q('spdSpark'), tr, 'spd', 30, t);
            box.querySelector('[data-act="follow"]').classList.toggle('on', sel.follow);
            box.querySelector('[data-act="trail"]').classList.toggle('on', aircraftTrails.has(sel.hex));
        }
        positionAnnotation();
    }

    function spark(svg, tr, key, h, t) {
        if (!svg) return;
        const f = tr ? tr.fixes.filter((x) => x.t <= t && x[key] !== null) : [];
        if (f.length < 2) {
            svg.innerHTML = `<text x="0" y="${h - 4}" class="spark-empty">Not enough recorded fixes yet</text>`;
            return;
        }
        let lo = Infinity, hi = -Infinity;
        for (const x of f) { lo = Math.min(lo, x[key]); hi = Math.max(hi, x[key]); }
        // a minimum span, so rounding noise does not look like a profile
        const minSpan = key === 'alt' ? 2000 : 40;
        if (hi - lo < minSpan) { const mid = (hi + lo) / 2; hi = mid + minSpan / 2; lo = mid - minSpan / 2; }
        const t0 = f[0].t, t1 = f[f.length - 1].t;
        const pts = f.map((x) => `${(((x.t - t0) / (t1 - t0 || 1)) * 240).toFixed(1)},${(h - 3 - ((x[key] - lo) / (hi - lo)) * (h - 8)).toFixed(1)}`).join(' ');
        svg.innerHTML = `<polyline points="${pts}"/><text x="240" y="9" text-anchor="end">${fmt(hi)}</text><text x="240" y="${h - 1}" text-anchor="end">${fmt(lo)}</text>`;
    }

    function positionAnnotation() {
        const box = $('annotation');
        const leader = $('annotation-leader');
        const m = markerOf(sel.hex);
        if (!sel.hex || !m || box.hidden) { leader.classList.remove('on'); return; }
        const size = map.getSize();
        const p = map.latLngToContainerPoint(m.getLatLng());
        if (sel.follow && !pb.dragging) {
            const c = map.getSize().divideBy(2);
            if (Math.hypot(p.x - c.x, p.y - c.y) > 60) map.panTo(m.getLatLng(), { animate: false });
        }
        const w = box.offsetWidth, h = box.offsetHeight;
        if (window.innerWidth <= 768) { leader.classList.remove('on'); return; }
        const right = p.x < size.x * 0.58;
        let x = right ? p.x + 70 : p.x - 70 - w;
        let y = p.y - 46;
        x = Math.max(64, Math.min(size.x - w - 72, x));
        y = Math.max(96, Math.min(size.y - h - 104, y));
        box.style.transform = `translate(${Math.round(x)}px, ${Math.round(y)}px)`;
        // leader: aircraft -> elbow -> the annotation's rule
        const ax = right ? x : x + w;
        const ay = y + 26;
        const ex = right ? ax - 18 : ax + 18;
        leader.querySelector('polyline').setAttribute('points', `${p.x},${p.y} ${ex},${ay} ${ax},${ay}`);
        leader.querySelector('circle').setAttribute('cx', p.x);
        leader.querySelector('circle').setAttribute('cy', p.y);
        leader.classList.add('on');
    }

    // -----------------------------------------------------------------------
    // Playback
    // -----------------------------------------------------------------------
    const pb = { active: false, t: null, playing: false, speed: 1, lastTick: 0, lastRender: 0, dragging: false, raf: 0 };
    const displayed = new Map(); // hex -> aircraft shown during playback
    let tailsLayer = null;
    let lastTails = 0;

    function displayTime() { return pb.active ? pb.t : Date.now(); }

    function bounds() {
        const now = Date.now();
        return { lo: Math.max(now - HISTORY_MS, rec.since ?? now), hi: now + FUTURE_MS, now };
    }

    function seek(t, { render = true } = {}) {
        const { lo, hi, now } = bounds();
        t = Math.max(lo, Math.min(hi, t));
        if (Math.abs(t - now) < 800 && !pb.dragging) { goLive(); return; }
        if (!pb.active) {
            pb.active = true;
            document.body.classList.add('is-playback');
        }
        pb.t = t;
        document.body.classList.toggle('is-projected', t > now);
        if (render) renderPlayback(true);
        timeline.update();
    }

    function renderPlayback(force = false) {
        const nowPerf = performance.now();
        if (!force && nowPerf - pb.lastRender < 90) return;
        pb.lastRender = nowPerf;
        const frame = frameAt(pb.t);
        displayed.clear();
        for (const a of frame) displayed.set(a.hex, a);
        updateAircraftOnMap(frame);
        drawTails(force);
        if (sel.hex) renderAnnotation();
    }

    function drawTails(force) {
        const nowPerf = performance.now();
        if (!force && nowPerf - lastTails < 250) return;
        lastTails = nowPerf;
        if (tailsLayer) tailsLayer.remove();
        tailsLayer = null;
        if (!pb.active || pb.t > Date.now()) return;
        const b = map.getBounds().pad(0.2);
        const g = L.layerGroup();
        const from = pb.t - 90000;
        for (const [hex, tr] of rec.tracks) {
            const cur = displayed.get(hex);
            if (!cur || !b.contains([cur.lat, cur.lon])) continue;
            const pts = [];
            for (const f of tr.fixes) if (f.t >= from && f.t <= pb.t) pts.push([f.lat, f.lon]);
            pts.push([cur.lat, cur.lon]);
            if (pts.length < 2) continue;
            L.polyline(pts, { pane: 'trailPane', color: PALETTE.alt[altitudeBand(cur)], weight: 1, opacity: 0.55, interactive: false }).addTo(g);
        }
        g.addTo(map);
        tailsLayer = g;
    }

    function goLive() {
        pb.active = false;
        pb.playing = false;
        pb.t = null;
        cancelAnimationFrame(pb.raf);
        document.body.classList.remove('is-playback', 'is-projected');
        displayed.clear();
        if (tailsLayer) { tailsLayer.remove(); tailsLayer = null; }
        updateAircraftOnMap(currentAircraftData);
        if (sel.hex) renderAnnotation();
        timeline.update();
    }

    function play() {
        if (!pb.active) seek(Date.now() - 2 * 60 * 1000);
        pb.playing = true;
        pb.lastTick = performance.now();
        cancelAnimationFrame(pb.raf);
        const tick = (ts) => {
            if (!pb.playing || !pb.active) return;
            const dt = ts - pb.lastTick;
            pb.lastTick = ts;
            const next = pb.t + dt * pb.speed;
            if (next >= Date.now() - 500 && pb.t <= Date.now()) { goLive(); return; }
            if (next >= bounds().hi) { pb.playing = false; timeline.update(); return; }
            pb.t = next;
            document.body.classList.toggle('is-projected', pb.t > Date.now());
            renderPlayback();
            timeline.update();
            pb.raf = requestAnimationFrame(tick);
        };
        pb.raf = requestAnimationFrame(tick);
        timeline.update();
    }

    function pause() {
        pb.playing = false;
        cancelAnimationFrame(pb.raf);
        timeline.update();
    }

    // -----------------------------------------------------------------------
    // Timeline (bottom margin of the map)
    // -----------------------------------------------------------------------
    const timeline = (() => {
        let canvas, ctx, tip, w = 0, h = 0, dpr = 1, hoverX = null;

        function x2t(x) { const now = Date.now(); return now - HISTORY_MS + (x / w) * (HISTORY_MS + FUTURE_MS); }
        function t2x(t) { const now = Date.now(); return ((t - (now - HISTORY_MS)) / (HISTORY_MS + FUTURE_MS)) * w; }

        function init() {
            canvas = $('timeline-canvas');
            ctx = canvas.getContext('2d');
            tip = $('timeline-tip');
            const resize = () => {
                dpr = window.devicePixelRatio || 1;
                w = canvas.clientWidth;
                h = canvas.clientHeight;
                canvas.width = Math.round(w * dpr);
                canvas.height = Math.round(h * dpr);
                draw();
            };
            new ResizeObserver(resize).observe(canvas);
            const scrub = (e) => {
                const r = canvas.getBoundingClientRect();
                seek(x2t(Math.max(0, Math.min(w, e.clientX - r.left))));
            };
            canvas.addEventListener('pointerdown', (e) => {
                const ev = nearestEvent(e);
                pb.dragging = true;
                canvas.setPointerCapture(e.pointerId);
                pause();
                if (ev && ev.hex && Math.abs(e.movementX) < 1) {
                    seek(ev.t);
                    if (rec.tracks.has(ev.hex)) setTimeout(() => select(ev.hex), 0);
                } else scrub(e);
            });
            canvas.addEventListener('pointermove', (e) => {
                const r = canvas.getBoundingClientRect();
                hoverX = e.clientX - r.left;
                if (pb.dragging) scrub(e);
                showTip(e);
                draw();
            });
            const end = (e) => {
                if (!pb.dragging) return;
                pb.dragging = false;
                try { canvas.releasePointerCapture(e.pointerId); } catch { /* released */ }
                if (pb.active && Math.abs(pb.t - Date.now()) < 1500) goLive();
            };
            canvas.addEventListener('pointerup', end);
            canvas.addEventListener('pointercancel', end);
            canvas.addEventListener('pointerleave', () => { hoverX = null; tip.hidden = true; draw(); });

            $('tl-play').onclick = () => (pb.playing ? pause() : play());
            $('tl-speed').onclick = () => {
                pb.speed = pb.speed === 1 ? 8 : pb.speed === 8 ? 32 : 1;
                timeline.update();
            };
            $('tl-live').onclick = () => goLive();
            setInterval(draw, 1000);
            update();
        }

        function nearestEvent(e) {
            const r = canvas.getBoundingClientRect();
            const x = e.clientX - r.left, y = e.clientY - r.top;
            if (y < h - 26) return null;
            let best = null, bd = 5;
            for (const ev of rec.events) {
                if (ev.kind === 'acquired' || ev.kind === 'lost') continue;
                const d = Math.abs(t2x(ev.t) - x);
                if (d < bd) { bd = d; best = ev; }
            }
            return best;
        }

        function showTip(e) {
            const r = canvas.getBoundingClientRect();
            const x = e.clientX - r.left;
            const t = x2t(x);
            const ev = nearestEvent(e);
            const now = Date.now();
            let text;
            if (ev) text = `<b>${clock(ev.t)}</b> ${esc(ev.text)}`;
            else if (t > now) text = `<b>${signedDuration(t - now)}</b> projected · constant track & speed`;
            else if (rec.since === null || t < rec.since) text = `<b>${clock(t)}</b> not recorded`;
            else {
                const c = nearestCount(t);
                const seeded = rec.seededUntil && t < rec.seededUntil;
                text = `<b>${clock(t)}</b> ${signedDuration(t - now)}${c ? ` · ${c.n} aircraft` : ''}${seeded ? ' · from gateway trails (aircraft still tracked at page load)' : ''}`;
            }
            tip.innerHTML = text;
            tip.hidden = false;
            const tw = tip.offsetWidth;
            tip.style.transform = `translateX(${Math.round(Math.max(0, Math.min(w - tw, x - tw / 2)))}px)`;
        }

        function nearestCount(t) {
            let best = null;
            for (const c of rec.counts) { if (c.t <= t) best = c; else break; }
            return best;
        }

        function draw() {
            if (!ctx || !w) return;
            const now = Date.now();
            const css = getComputedStyle(document.documentElement);
            const col = (v) => css.getPropertyValue(v).trim();
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            ctx.clearRect(0, 0, w, h);
            const axisY = h - 18;
            const traceTop = 6, traceBottom = axisY - 8;
            const nowX = t2x(now);

            // not recorded (before the page/gateway history)
            const recX = rec.since === null ? nowX : Math.max(0, t2x(rec.since));
            if (recX > 1) {
                ctx.fillStyle = col('--stone-2');
                for (let x = 0; x < recX; x += 3) ctx.fillRect(x, axisY - 0.5, 1, 1);
                if (recX > 110) {
                    ctx.font = `500 9px ${col('--font-cond')}`;
                    ctx.fillStyle = col('--stone-2');
                    ctx.textAlign = 'left';
                    ctx.fillText('NOT RECORDED', 2, traceBottom - 2);
                }
            }
            // gateway-seeded stretch: partial coverage, say so
            if (rec.seededUntil) {
                const sx = Math.max(recX, 0), ex = Math.min(t2x(rec.seededUntil), nowX);
                if (ex - sx > 90) {
                    ctx.font = `500 9px ${col('--font-cond')}`;
                    ctx.fillStyle = col('--stone-2');
                    ctx.textAlign = 'left';
                    ctx.fillText('GATEWAY TRAILS · PARTIAL', sx + 4, traceBottom - 2);
                }
            }
            // projected future: hatching
            ctx.save();
            ctx.beginPath();
            ctx.rect(nowX, traceTop, w - nowX, axisY - traceTop);
            ctx.clip();
            ctx.strokeStyle = col('--line-strong');
            ctx.lineWidth = 1;
            for (let x = nowX - h; x < w; x += 6) { ctx.beginPath(); ctx.moveTo(x, axisY); ctx.lineTo(x + (axisY - traceTop), traceTop); ctx.stroke(); }
            ctx.restore();
            ctx.font = `500 9px ${col('--font-cond')}`;
            ctx.fillStyle = col('--stone');
            ctx.textAlign = 'left';
            ctx.fillText('PROJECTED', nowX + 8, traceTop + 9);

            // recorded aircraft count
            const counts = rec.counts.filter((c) => c.t >= now - HISTORY_MS);
            if (counts.length > 1) {
                const max = Math.max(1, ...counts.map((c) => c.n));
                ctx.beginPath();
                counts.forEach((c, i) => {
                    const x = t2x(c.t), y = traceBottom - (c.n / max) * (traceBottom - traceTop - 12);
                    if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
                });
                ctx.lineTo(nowX, traceBottom - (counts[counts.length - 1].n / max) * (traceBottom - traceTop - 12));
                ctx.strokeStyle = col('--ink-2');
                ctx.lineWidth = 1;
                // seeded stretch (before page load) lighter than what the page saw itself
                const splitX = rec.seededUntil ? t2x(rec.seededUntil) : -1;
                ctx.save();
                ctx.beginPath(); ctx.rect(0, 0, Math.max(0, splitX), h); ctx.clip();
                ctx.globalAlpha = 0.35; ctx.stroke();
                ctx.restore();
                ctx.save();
                ctx.beginPath(); ctx.rect(Math.max(0, splitX), 0, w, h); ctx.clip();
                ctx.globalAlpha = 0.8; ctx.stroke();
                ctx.restore();
                ctx.globalAlpha = 1;
                ctx.fillStyle = col('--stone');
                ctx.textAlign = 'left';
                ctx.fillText(`${max}`, Math.max(recX, 0) + 2, traceTop + 9);
            }

            // axis + ticks
            ctx.fillStyle = col('--line-strong');
            ctx.fillRect(Math.max(0, recX), axisY, nowX - Math.max(0, recX), 1);
            ctx.fillStyle = col('--line');
            ctx.fillRect(nowX, axisY, w - nowX, 1);
            ctx.font = `400 9.5px ${col('--font-mono')}`;
            ctx.textAlign = 'center';
            for (let k = -15; k <= 5; k++) {
                const x = t2x(now + k * 60000);
                const major = k % 5 === 0;
                ctx.fillStyle = major ? col('--stone') : col('--line-strong');
                ctx.fillRect(Math.round(x), axisY, 1, major ? 6 : 3);
                if (major && k !== 0) {
                    ctx.fillStyle = col('--stone');
                    ctx.fillText(`${k > 0 ? '+' : '−'}${Math.abs(k)}′`, x, h - 2);
                }
            }

            // events
            for (const ev of rec.events) {
                if (ev.t < now - HISTORY_MS) continue;
                const x = Math.round(t2x(ev.t));
                if (ev.kind === 'acquired') { ctx.fillStyle = col('--stone-2'); ctx.fillRect(x, axisY - 5, 1, 4); }
                else if (ev.kind === 'lost') { ctx.fillStyle = col('--stone-2'); ctx.fillRect(x, axisY + 2, 1, 4); }
                else if (ev.kind === 'alert') {
                    ctx.fillStyle = col('--alert');
                    ctx.fillRect(x, traceTop, 1, axisY - traceTop);
                    ctx.beginPath(); ctx.arc(x, axisY, 3, 0, Math.PI * 2); ctx.fill();
                } else {
                    ctx.fillStyle = col('--ochre');
                    ctx.beginPath(); ctx.moveTo(x, axisY - 4); ctx.lineTo(x + 3.5, axisY); ctx.lineTo(x, axisY + 4); ctx.lineTo(x - 3.5, axisY); ctx.closePath(); ctx.fill();
                }
            }

            // now
            ctx.fillStyle = col('--ink');
            ctx.fillRect(Math.round(nowX), traceTop - 2, 1, axisY - traceTop + 8);
            ctx.font = `500 9px ${col('--font-cond')}`;
            ctx.textAlign = 'center';
            ctx.fillText('NOW', nowX, h - 2);

            // playhead
            if (pb.active) {
                const x = Math.round(t2x(pb.t));
                ctx.fillStyle = col('--ochre');
                ctx.fillRect(x, 0, 1.5, axisY + 6);
                ctx.beginPath(); ctx.moveTo(x - 4.5, 0); ctx.lineTo(x + 6, 0); ctx.lineTo(x + 0.75, 6); ctx.closePath(); ctx.fill();
            }
            if (hoverX !== null && !pb.dragging) {
                ctx.fillStyle = col('--stone-2');
                ctx.fillRect(Math.round(hoverX), traceTop, 1, axisY - traceTop);
            }
        }

        function update() {
            const now = Date.now();
            const playBtn = $('tl-play');
            playBtn.classList.toggle('playing', pb.playing);
            playBtn.setAttribute('aria-label', pb.playing ? 'Pause' : 'Play back');
            $('tl-speed').textContent = `${pb.speed}×`;
            $('tl-live').classList.toggle('on', !pb.active);
            const big = $('tl-time');
            const sub = $('tl-sub');
            if (!pb.active) {
                big.textContent = 'Live';
                sub.textContent = rec.since ? `recorded since ${clock(rec.since)}` : 'recording';
            } else {
                big.textContent = signedDuration(pb.t - now);
                sub.textContent = pb.t > now ? 'projected · not observed' : `recorded · ${clock(pb.t)}`;
            }
            draw();
        }

        return { init, draw, update };
    })();

    // -----------------------------------------------------------------------
    // Altitude tape (right edge): the legend and the distribution in one
    // -----------------------------------------------------------------------
    const tape = (() => {
        let canvas, ctx, w = 0, h = 0, dpr = 1, last = 0;
        const TOP = 45000;
        function init() {
            canvas = $('alt-tape-canvas');
            ctx = canvas.getContext('2d');
            new ResizeObserver(() => {
                dpr = window.devicePixelRatio || 1;
                w = canvas.clientWidth; h = canvas.clientHeight;
                canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
                draw(true);
            }).observe(canvas);
        }
        function draw(force = false) {
            if (!ctx || !w) return;
            const t = performance.now();
            if (!force && t - last < 250) return;
            last = t;
            const css = getComputedStyle(document.documentElement);
            const col = (v) => css.getPropertyValue(v).trim();
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            ctx.clearRect(0, 0, w, h);
            const axisX = w - 34;
            const yOf = (alt) => 8 + (1 - Math.min(alt, TOP) / TOP) * (h - 30);
            // band segments along the axis
            const bands = [['low', 0, 10000], ['mid', 10000, 20000], ['high', 20000, 30000], ['top', 30000, TOP]];
            for (const [b, lo, hi] of bands) {
                ctx.fillStyle = col(`--alt-${b}`);
                ctx.fillRect(axisX, yOf(hi), 1.5, yOf(lo) - yOf(hi));
            }
            ctx.font = `400 9.5px ${col('--font-mono')}`;
            ctx.textAlign = 'left';
            for (let alt = 0; alt <= 40000; alt += 5000) {
                const y = Math.round(yOf(alt));
                ctx.fillStyle = col('--stone-2');
                ctx.fillRect(axisX + 2, y, alt % 10000 === 0 ? 6 : 3, 1);
                if (alt % 10000 === 0) {
                    ctx.fillStyle = col('--stone');
                    ctx.fillText(alt === 0 ? 'SFC' : `${alt / 100}`, axisX + 11, y + 3);
                }
            }
            // ground
            const gy = h - 10;
            ctx.fillStyle = col('--alt-ground');
            ctx.fillRect(axisX, gy, 1.5, 4);
            ctx.fillStyle = col('--stone');
            ctx.fillText('GND', axisX + 11, gy + 5);
            // aircraft
            const list = pb.active ? [...displayed.values()] : currentAircraftData;
            let selY = null, sel_ = null;
            ctx.globalAlpha = 0.7;
            for (const a of list) {
                const band = altitudeBand(a);
                const y = a.on_ground ? gy + 1 : a.altitude === null || a.altitude === undefined ? null : yOf(a.altitude);
                if (y === null) continue;
                if (a.hex === sel.hex) { selY = y; sel_ = a; continue; }
                ctx.fillStyle = col(`--alt-${band}`);
                ctx.fillRect(axisX - 12 + ((Number.parseInt(a.hex, 16) % 5)), Math.round(y), 9, 1);
            }
            ctx.globalAlpha = 1;
            if (selY !== null) {
                ctx.fillStyle = col('--ink');
                ctx.fillRect(axisX - 26, Math.round(selY), 24, 1.5);
                ctx.font = `500 10px ${col('--font-mono')}`;
                ctx.textAlign = 'right';
                ctx.fillText(sel_.flight || sel_.hex, axisX - 30, selY + 3.5);
            }
        }
        return { init, draw };
    })();

    // -----------------------------------------------------------------------
    // Readouts, masthead, scale
    // -----------------------------------------------------------------------
    function onStatus(connected, text) {
        const prev = live.statusText;
        live.connected = connected;
        live.statusText = text;
        if (prev !== text && prev !== 'Connecting' && text !== 'Connecting...') {
            event(Date.now(), connected ? 'feed' : 'alert-feed', null, `Feed · ${text}`);
        }
        const v = $('ro-link-value');
        const sub = $('ro-link-sub');
        if (!v) return;
        if (connected) {
            v.textContent = 'Live';
            sub.textContent = liveFeed?.synced ? 'websocket' : 'polling · 2 s';
        } else {
            v.textContent = text.replace(/\.\.\.$/, '…');
            sub.textContent = liveFeed?.connected ? 'websocket' : 'polling · 2 s';
        }
        $('ro-link').classList.toggle('is-down', !connected);
    }

    function onCollisions(data) {
        const prev = live.collisions;
        live.collisions = data.collisions ? data.collisions.length : 0;
        live.collisionNote = data.note || null;
        const v = $('collision-count');
        const sub = $('ro-conflict-sub');
        const ro = $('ro-conflict');
        if (data.note) {
            // the gateway does not run collision detection: say so, not "0"
            v.textContent = '—';
            sub.textContent = 'not computed here';
            ro.title = data.note;
        } else {
            sub.textContent = 'next 5 min';
            ro.title = 'Kalman-filter conflict prediction (Fall 2025 server)';
        }
        ro.classList.toggle('is-alert', !data.note && live.collisions > 0);
        if (!data.note && live.collisions > 0 && !prev) {
            const c = data.collisions[0];
            event(Date.now(), 'alert', c.aircraft1, `Conflict predicted · ${c.flight1 || c.aircraft1} / ${c.flight2 || c.aircraft2}`);
        }
    }

    async function pollStats() {
        if (live.statsAvailable === false) return;
        try {
            const r = await fetch('/gateway/stats', { cache: 'no-store' });
            if (!r.ok) throw new Error(String(r.status));
            const s = await r.json();
            const t = performance.now();
            if (live.stats && live.statsT) {
                const d = (s.subscriber?.applied ?? 0) - (live.stats.subscriber?.applied ?? 0);
                live.rate = d >= 0 ? d / ((t - live.statsT) / 1000) : null;
            }
            live.stats = s;
            live.statsT = t;
            live.statsAvailable = true;
            AtlasAnalysis.ingestStats(s);
            document.body.classList.add('has-feed-stats');
            $('ro-seq-value').textContent = fmt(s.status?.as_of_seq);
            $('ro-seq-sub').textContent = `session ${s.status?.session ?? '—'}`;
            $('ro-rate-value').textContent = live.rate === null ? '—' : fmt(live.rate);
            const lat = s.latency_us?.feed_to_ws;
            $('ro-lat-value').textContent = lat?.count ? fmt(lat.p50_us) : '—';
            $('ro-lat-sub').textContent = lat?.count ? `µs p50 · p99 ${fmt(lat.p99_us)}` : 'µs p50';
        } catch {
            if (live.statsAvailable === null) {
                live.statsAvailable = false;
                document.body.classList.remove('has-feed-stats');
            }
        }
    }

    function updateReadouts() {
        // aircraft in view / tracked
        const b = map.getBounds();
        let inView = 0;
        const list = pb.active ? [...displayed.values()] : currentAircraftData;
        for (const a of list) if (a.lat !== null && b.contains([a.lat, a.lon])) inView++;
        $('count-big').textContent = fmt(inView);
        $('count-sub').textContent = pb.active ? `of ${fmt(list.length)} at display time` : `of ${fmt(live.tracked)} tracked`;
        const age = live.lastData ? Date.now() - live.lastData : null;
        $('ro-update-sub').textContent = age === null ? 'waiting' : age < 1500 ? 'just now' : `${(age / 1000).toFixed(0)} s ago`;
    }

    function updateMasthead() {
        const c = map.getCenter();
        $('mast-coords').textContent = coords(c.lat, c.lng);
        $('mast-zoom').textContent = `Z${map.getZoom()}`;
        const feats = window.airportsData?.features || (typeof airportsData !== 'undefined' && airportsData?.features) || null;
        if (!feats) return;
        let best = null, bd = Infinity;
        for (const f of feats) {
            if (f.properties.type !== 'large_airport' && f.properties.type !== 'medium_airport') continue;
            const [lon, lat] = f.geometry.coordinates;
            const d = map.distance(c, [lat, lon]) * (f.properties.type === 'large_airport' ? 0.55 : 1);
            if (d < bd) { bd = d; best = f; }
        }
        if (best && bd < 250000) {
            $('mast-place').textContent = (best.properties.municipality || best.properties.name).split(' / ')[0];
            $('mast-near').textContent = `nearest ${shortAirport(best.properties.name)}`;
        } else {
            $('mast-place').textContent = 'Open airspace';
            $('mast-near').textContent = '';
        }
    }

    function shortAirport(name) {
        return name.replace(/ International Airport$| Airport$| International$/, '').replace(/ \/.*$/, '');
    }

    function updateScale() {
        const c = map.getCenter();
        const y = map.getSize().y / 2;
        const p1 = map.containerPointToLatLng([0, y]);
        const p2 = map.containerPointToLatLng([100, y]);
        const mPerPx = map.distance(p1, p2) / 100;
        const target = 120 * mPerPx / NM;
        const steps = [0.5, 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500];
        let nm = steps[0];
        for (const s of steps) if (s <= target) nm = s;
        const px = (nm * NM) / mPerPx;
        $('scale-bar').style.width = `${Math.round(px)}px`;
        $('scale-label').textContent = `${nm} NM`;
        $('scale-mid').textContent = nm / 2 >= 1 ? `${nm / 2}` : '';
        void c;
    }

    // Large-airport names in the cartographic serif
    let aptLabels = null;
    function drawAirportNames() {
        const feats = typeof airportsData !== 'undefined' && airportsData?.features;
        if (!feats) return;
        if (aptLabels) aptLabels.remove();
        aptLabels = L.layerGroup();
        if (map.getZoom() >= 7) {
            const b = map.getBounds().pad(0.1);
            for (const f of feats) {
                if (f.properties.type !== 'large_airport') continue;
                const [lon, lat] = f.geometry.coordinates;
                if (!b.contains([lat, lon])) continue;
                L.marker([lat, lon], {
                    interactive: false, keyboard: false, pane: 'trailPane',
                    icon: L.divIcon({
                        className: 'apt-name',
                        html: `<i class="apt-large"></i><span>${esc(shortAirport(f.properties.name))}</span><em>${esc(f.properties.icao || '')}${f.properties.elevation ? ` · ${f.properties.elevation} ft` : ''}</em>`,
                        iconSize: [0, 0],
                    }),
                }).addTo(aptLabels);
            }
        }
        aptLabels.addTo(map);
    }

    // -----------------------------------------------------------------------
    // Rail and sheets
    // -----------------------------------------------------------------------
    const sheet = { current: null, sort: 'callsign', query: '', lastIndex: 0, stats: null };

    function openSheet(name) {
        if (sheet.current === name) name = null;
        sheet.current = name;
        for (const b of document.querySelectorAll('.rail-btn[data-sheet]')) b.classList.toggle('active', b.dataset.sheet === name);
        for (const s of document.querySelectorAll('.sheet')) s.hidden = s.dataset.sheet !== name;
        document.body.classList.toggle('has-sheet', !!name);
        if (name === 'index') { renderIndex(currentAircraftData, true); setTimeout(() => $('index-search').focus(), 0); }
        AtlasAnalysis.show(name === 'analysis');
        scheduleLayout(true);
    }

    function renderIndex(list, force = false) {
        if (sheet.current !== 'index') return;
        const t = performance.now();
        if (!force && t - sheet.lastIndex < 500) return;
        sheet.lastIndex = t;
        const q = sheet.query.trim().toUpperCase();
        let rows = (pb.active ? [...displayed.values()] : list).filter((a) => !q || (a.flight || '').toUpperCase().includes(q) || a.hex.includes(q) || (a.squawk || '').includes(q));
        const by = {
            callsign: (a, b) => (a.flight || a.hex).localeCompare(b.flight || b.hex),
            alt: (a, b) => (b.altitude ?? -1) - (a.altitude ?? -1),
            spd: (a, b) => (b.speed ?? -1) - (a.speed ?? -1),
        }[sheet.sort];
        rows = rows.slice().sort(by);
        $('index-count').textContent = `${rows.length} of ${(pb.active ? displayed.size : list.length)}`;
        $('index-list').innerHTML = rows.map((a) => {
            const band = altitudeBand(a);
            const emergency = EMERGENCY[a.squawk];
            return `<li class="index-row${a.hex === sel.hex ? ' is-selected' : ''}${a.hex === hov.hex ? ' is-hover' : ''}${emergency ? ' is-emergency' : ''}" data-hex="${a.hex}" data-band="${band}">
                <i></i><b>${esc(a.flight || a.hex)}</b><span>${a.on_ground ? 'GND' : a.altitude === null ? '—' : fmt(a.altitude)}</span><span>${a.speed ?? '—'}</span><span>${a.track === null ? '—' : String(a.track).padStart(3, '0')}</span></li>`;
        }).join('') || '<li class="index-empty">No aircraft match</li>';
    }

    function renderAnalysis(data) {
        AtlasAnalysis.setStatistics(data);
    }

    function initRail() {
        for (const b of document.querySelectorAll('.rail-btn[data-sheet]')) b.onclick = () => openSheet(b.dataset.sheet);
        for (const b of document.querySelectorAll('.sheet-close')) b.onclick = () => openSheet(null);
        $('rail-zoom-in').onclick = () => map.zoomIn();
        $('rail-zoom-out').onclick = () => map.zoomOut();
        $('rail-home').onclick = () => map.flyTo(RECEIVER, 9, { duration: 0.6 });
        $('index-search').addEventListener('input', (e) => { sheet.query = e.target.value; renderIndex(currentAircraftData, true); });
        for (const b of document.querySelectorAll('.index-sort button')) {
            b.onclick = () => {
                sheet.sort = b.dataset.sort;
                for (const o of document.querySelectorAll('.index-sort button')) o.classList.toggle('on', o === b);
                renderIndex(currentAircraftData, true);
            };
        }
        const list = $('index-list');
        list.addEventListener('mouseover', (e) => { const r = e.target.closest('.index-row'); if (r) hover(r.dataset.hex); });
        list.addEventListener('mouseleave', () => hover(null));
        list.addEventListener('click', (e) => { const r = e.target.closest('.index-row'); if (r) select(r.dataset.hex, { fly: true }); });
        $('toggle-labels').onclick = (e) => {
            labelsOn = !labelsOn;
            e.currentTarget.classList.toggle('active', labelsOn);
            scheduleLayout(true);
        };
        $('toggle-wind').onclick = (e) => {
            const on = !e.currentTarget.classList.contains('active');
            e.currentTarget.classList.toggle('active', on);
            document.querySelector('.wind-levels').classList.toggle('off', !on);
            AtlasWind.setEnabled(on);
        };
        for (const b of document.querySelectorAll('.wind-levels button')) {
            b.onclick = () => {
                for (const o of document.querySelectorAll('.wind-levels button')) o.classList.toggle('on', o === b);
                AtlasWind.setLevel(b.dataset.level);
            };
        }
        $('toggle-tape').onclick = (e) => {
            const on = document.body.classList.toggle('no-tape');
            e.currentTarget.classList.toggle('active', !on);
        };
    }

    // -----------------------------------------------------------------------
    // Keyboard
    // -----------------------------------------------------------------------
    function initKeys() {
        document.addEventListener('keydown', (e) => {
            if (e.target instanceof Element && e.target.matches('input, textarea')) {
                if (e.key === 'Escape') { e.target.blur(); openSheet(null); }
                return;
            }
            if (e.key === 'Escape') {
                if (sel.hex) select(null);
                else if (sheet.current) openSheet(null);
                else if (pb.active) goLive();
            } else if (e.key === ' ') { e.preventDefault(); pb.playing ? pause() : play(); }
            else if (e.key === 'ArrowLeft') { e.preventDefault(); pause(); seek((pb.active ? pb.t : Date.now()) - (e.shiftKey ? 60000 : 10000)); }
            else if (e.key === 'ArrowRight' && pb.active) { e.preventDefault(); pause(); seek(pb.t + (e.shiftKey ? 60000 : 10000)); }
            else if (e.key === 'l' || e.key === 'L') goLive();
            else if (e.key === '/') { e.preventDefault(); if (sheet.current !== 'index') openSheet('index'); }
            else if (e.key === 'n' || e.key === 'N') labelsOn = !labelsOn, $('toggle-labels').classList.toggle('active', labelsOn), scheduleLayout(true);
        });
    }

    // -----------------------------------------------------------------------
    // Wiring
    // -----------------------------------------------------------------------
    function afterMapUpdate() {
        for (const [hex, m] of aircraftMarkers) {
            if (m._atlasInit || !m._icon) continue;
            m._atlasInit = true;
            m._lpos = undefined;
            if (hex === sel.hex) setMarkerClass(hex, 'is-selected', true);
        }
        scheduleLayout();
        tape.draw();
        if (sel.hex && !pb.active) renderAnnotation();
        if (sheet.current === 'index' && pb.active) renderIndex(currentAircraftData);
    }

    function init(m) {
        map = m;
        initRail();
        initKeys();
        timeline.init();
        tape.init();
        AtlasWind.init(map, { legend: $('wind-legend') });
        AtlasAnalysis.init({
            getAircraft: () => currentAircraftData,
            receiver: RECEIVER,
            page: () => (typeof liveFeed !== 'undefined' && liveFeed.connected ? liveFeed : null),
            onSelect: (hex) => { if (aircraftMarkers.has(hex)) select(hex, { fly: true }); },
        }, $('analysis-body'));
        map.on('click', () => { if (sel.hex) select(null); });
        map.on('zoomend moveend', () => {
            scheduleLayout(true);
            updateMasthead();
            updateScale();
            drawAirportNames();
            updateReadouts();
            if (pb.active) drawTails(true);
        });
        map.on('move zoom', positionAnnotation);
        map.on('zoomstart', () => document.body.classList.add('is-zooming'));
        map.on('zoomend', () => document.body.classList.remove('is-zooming'));
        window.addEventListener('resize', () => scheduleLayout(true));
        setInterval(() => { updateReadouts(); if (!pb.active) timeline.update(); }, 1000);
        setInterval(pollStats, 1000);
        pollStats();
        seedFromGateway();
        updateMasthead();
        updateScale();
        // airports load asynchronously in app.js
        const waitAirports = setInterval(() => {
            if (typeof airportsData !== 'undefined' && airportsData) {
                clearInterval(waitAirports);
                updateMasthead();
                drawAirportNames();
            }
        }, 300);
    }

    return {
        init, record, isPlayback: () => pb.active, afterMapUpdate, select, hover,
        renderIndex, renderAnalysis, onStatus, onCollisions,
        // for tests and measurement
        _state: { rec, pb, sel, live, frameAt, seek, goLive, layout },
    };
})();
