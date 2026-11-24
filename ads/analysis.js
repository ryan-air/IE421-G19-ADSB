// ---------------------------------------------------------------------------
// Analysis sheet, shared by the map (index.html) and the 3D view (3d.html).
//
// Everything here is measured, nowhere estimated:
//   - /gateway/stats (passed in by the page, about once a second): feed
//     integrity counters, fan-out counters, cumulative latency percentiles,
//     clock calibration;
//   - /gateway/latency (polled here every 5 s): cumulative log-bucket latency
//     histograms; two readings 5 s apart give the distribution and the
//     percentiles of that interval alone;
//   - the page's own aircraft list and, on the live map, its WebSocket
//     counters (frames, bytes, updates per aircraft, redraw time).
// Served by the Fall 2025 server there is no gateway: the system tab says so
// and the traffic tab still works from the aircraft list.
// ---------------------------------------------------------------------------
const AtlasAnalysis = (() => {
    const HISTORY_MS = 15 * 60 * 1000;
    const LAT_POLL_MS = 5000;
    const TRAFFIC_MS = 5000;
    const EMERGENCY = { '7500': 'Unlawful interference', '7600': 'Radio failure', '7700': 'General emergency' };

    const st = {
        host: null,
        open: false,
        tab: 'system',
        histWindow: 'minute', // 'minute' | 'all'
        body: null,
        startedAt: Date.now(),
        stats: null,
        statsSamples: [], // {t, applied, frames, bytes, clients}
        pageSamples: [], // {t, frames, bytes, updates, renders}
        lat: { available: null, edges: null, snaps: [], series: [] },
        statistics: null,
        coverage: new Float64Array(36), // furthest position seen per 10° sector, NM
        coverageAt: new Float64Array(36), // when
        firstSeen: new Map(), // hex -> ms
        updSnaps: [], // {t, counts: Map icao -> updates}
        lastRender: 0,
    };

    // ---- formatting -------------------------------------------------------

    const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    const fmt = (n, d = 0, min = d) => (n === null || n === undefined || Number.isNaN(n) ? '—' : Number(n).toLocaleString('en-US', { maximumFractionDigits: d, minimumFractionDigits: min }));
    // a latency in µs, with a unit that keeps three significant figures
    function us(v) {
        if (v === null || v === undefined || Number.isNaN(v)) return { v: '—', u: '' };
        if (!Number.isFinite(v)) return { v: '≥ 1', u: 's' };
        if (v < 1000) return { v: fmt(v, v < 10 ? 1 : 0, 0), u: 'µs' };
        if (v < 1e6) return { v: fmt(v / 1000, v < 1e4 ? 2 : v < 1e5 ? 1 : 0, 0), u: 'ms' };
        return { v: fmt(v / 1e6, 2, 0), u: 's' };
    }
    const usText = (v) => { const x = us(v); return `${x.v} ${x.u}`.trim(); };
    function bytes(n) {
        if (!n) return '0 B';
        const u = ['B', 'kB', 'MB', 'GB'];
        let i = 0;
        while (n >= 1000 && i < u.length - 1) { n /= 1000; i++; }
        return `${fmt(n, i === 0 ? 0 : n < 10 ? 2 : 1)} ${u[i]}`;
    }
    function duration(ms) {
        const s = Math.max(0, Math.round(ms / 1000));
        if (s < 60) return `${s} s`;
        if (s < 3600) return `${Math.floor(s / 60)} min ${String(s % 60).padStart(2, '0')} s`;
        return `${Math.floor(s / 3600)} h ${String(Math.floor((s % 3600) / 60)).padStart(2, '0')} min`;
    }
    const clock = (t) => new Date(t).toLocaleTimeString('en-GB', { hour12: false });
    const band = (a) => (a.on_ground ? 'ground' : a.altitude === null || a.altitude === undefined ? 'mid' : a.altitude < 10000 ? 'low' : a.altitude < 20000 ? 'mid' : a.altitude < 30000 ? 'high' : 'top');

    // ---- log-bucket percentiles ---------------------------------------------

    // q-quantile of a log-bucket histogram, interpolated geometrically inside
    // the bucket (linearly in the 1 µs buckets). Infinity = past 1 s.
    function quantile(counts, over, q) {
        const edges = st.lat.edges;
        let total = over;
        for (const c of counts) total += c;
        if (!total) return null;
        const rank = q * total;
        let seen = 0;
        for (let i = 0; i < counts.length; i++) {
            const c = counts[i];
            if (c > 0 && seen + c >= rank) {
                const f = Math.min(1, Math.max(0, (rank - seen) / c));
                const lo = edges[i];
                const hi = edges[i + 1];
                return lo < 4 ? lo + f * (hi - lo) : lo * (hi / lo) ** f;
            }
            seen += c;
        }
        return Infinity;
    }
    const sum = (a) => a.reduce((x, y) => x + y, 0);
    const diff = (a, b) => a.map((v, i) => Math.max(0, v - (b ? b[i] : 0)));

    async function pollLatency() {
        if (st.lat.available === false) return;
        try {
            const r = await fetch('/gateway/latency', { cache: 'no-store' });
            if (!r.ok) throw new Error(String(r.status));
            const d = await r.json();
            st.lat.available = true;
            st.lat.edges = d.feed_to_ws.edges_us;
            const snap = {
                t: Date.now(),
                g: d.feed_to_gateway.counts, gOver: d.feed_to_gateway.over_1s,
                w: d.feed_to_ws.counts, wOver: d.feed_to_ws.over_1s,
            };
            const prev = st.lat.snaps[st.lat.snaps.length - 1];
            st.lat.snaps.push(snap);
            if (prev) {
                const g = diff(snap.g, prev.g);
                const w = diff(snap.w, prev.w);
                const gOver = Math.max(0, snap.gOver - prev.gOver);
                const wOver = Math.max(0, snap.wOver - prev.wOver);
                st.lat.series.push({
                    t: snap.t,
                    n: sum(w) + wOver,
                    g50: quantile(g, gOver, 0.5), g99: quantile(g, gOver, 0.99),
                    w50: quantile(w, wOver, 0.5), w99: quantile(w, wOver, 0.99),
                });
            }
            prune(st.lat.snaps);
            prune(st.lat.series);
        } catch {
            if (st.lat.available === null) st.lat.available = false;
        }
    }

    function prune(arr, keep = HISTORY_MS) {
        const cut = Date.now() - keep;
        let i = 0;
        while (i < arr.length - 1 && arr[i].t < cut) i++;
        if (i) arr.splice(0, i);
    }

    // ---- sampling -------------------------------------------------------------

    function ingestStats(s) {
        if (!s) return;
        st.stats = s;
        st.statsSamples.push({
            t: Date.now(),
            applied: s.subscriber?.applied ?? 0,
            frames: s.fanout?.frames ?? 0,
            bytes: s.fanout?.bytes ?? 0,
            clients: s.status?.clients ?? 0,
        });
        prune(st.statsSamples);
        if (st.open && st.tab === 'system' && Date.now() - st.lastRender > 1800) render();
    }

    function samplePage() {
        const p = st.host?.page?.();
        if (!p) return;
        st.pageSamples.push({ t: Date.now(), frames: p.frames, bytes: p.bytes, updates: p.updates, renders: p.renders });
        prune(st.pageSamples);
    }

    function haversine(lat1, lon1, lat2, lon2) {
        const R = 3440.065; // NM
        const toR = Math.PI / 180;
        const dLat = (lat2 - lat1) * toR;
        const dLon = (lon2 - lon1) * toR;
        const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * toR) * Math.cos(lat2 * toR) * Math.sin(dLon / 2) ** 2;
        const nm = 2 * R * Math.asin(Math.min(1, Math.sqrt(a)));
        const y = Math.sin(dLon) * Math.cos(lat2 * toR);
        const x = Math.cos(lat1 * toR) * Math.sin(lat2 * toR) - Math.sin(lat1 * toR) * Math.cos(lat2 * toR) * Math.cos(dLon);
        const brg = (Math.atan2(y, x) / toR + 360) % 360;
        return { nm, brg };
    }

    function sampleTraffic() {
        const list = st.host?.getAircraft?.() || [];
        const [rlat, rlon] = st.host?.receiver || [0, 0];
        const now = Date.now();
        for (const a of list) {
            if (!st.firstSeen.has(a.hex)) st.firstSeen.set(a.hex, now);
            if (a.lat === null || a.lon === null || a.lat === undefined) continue;
            const { nm, brg } = haversine(rlat, rlon, a.lat, a.lon);
            const s = Math.floor(brg / 10) % 36;
            if (nm > st.coverage[s]) { st.coverage[s] = nm; st.coverageAt[s] = now; }
        }
        const p = st.host?.page?.();
        if (p?.updatesBy) {
            st.updSnaps.push({ t: now, counts: new Map(p.updatesBy) });
            prune(st.updSnaps, 45_000);
        }
        if (st.open && st.tab === 'traffic') render();
    }

    // ---- charts (inline SVG) --------------------------------------------------

    const W = 352;

    // Distribution of one interval on a log time axis: feed → WebSocket as
    // bars, feed → gateway as a stepped outline.
    function histogramSvg(w, wOver, g, gOver) {
        const H = 128, top = 14, bottom = 22, left = 2, right = 4;
        const edges = st.lat.edges;
        const lx = (v) => Math.log10(Math.max(0.5, v));
        const x0 = lx(0.5), x1 = lx(1e6);
        const X = (v) => left + ((lx(v) - x0) / (x1 - x0)) * (W - left - right);
        const ph = H - top - bottom;
        const tw = sum(w) + wOver, tg = sum(g) + gOver;
        // density per log width, so the 1 µs buckets do not tower over the rest
        const dens = (c, i, t) => (t ? c / t / (lx(edges[i + 1]) - lx(Math.max(0.5, edges[i]))) : 0);
        let max = 0;
        for (let i = 0; i < w.length; i++) max = Math.max(max, dens(w[i], i, tw), dens(g[i], i, tg));
        if (!max) return `<svg class="an-chart" viewBox="0 0 ${W} ${H}"><text x="${W / 2}" y="${H / 2}" text-anchor="middle" class="ch-empty">No samples in this interval</text></svg>`;
        const Y = (d) => top + ph - (d / max) * ph;
        let bars = '';
        for (let i = 0; i < w.length; i++) {
            if (!w[i]) continue;
            const xa = X(Math.max(0.5, edges[i])), xb = X(edges[i + 1]);
            bars += `<rect x="${xa.toFixed(1)}" y="${Y(dens(w[i], i, tw)).toFixed(1)}" width="${Math.max(0.8, xb - xa - 0.6).toFixed(1)}" height="${(top + ph - Y(dens(w[i], i, tw))).toFixed(1)}"/>`;
        }
        let step = '';
        let first = -1, last = -1;
        for (let i = 0; i < g.length; i++) if (g[i]) { if (first < 0) first = i; last = i; }
        if (first >= 0) {
            step = `M${X(Math.max(0.5, edges[first])).toFixed(1)} ${top + ph}`;
            for (let i = first; i <= last; i++) {
                const y = Y(dens(g[i], i, tg)).toFixed(1);
                step += ` L${X(Math.max(0.5, edges[i])).toFixed(1)} ${y} L${X(edges[i + 1]).toFixed(1)} ${y}`;
            }
            step += ` L${X(edges[last + 1]).toFixed(1)} ${top + ph}`;
        }
        const ticks = [[1, '1 µs'], [10, '10'], [100, '100'], [1e3, '1 ms'], [1e4, '10'], [1e5, '100'], [1e6, '1 s']]
            .map(([v, l]) => `<line x1="${X(v)}" x2="${X(v)}" y1="${top + ph}" y2="${top + ph + 4}" class="ch-tick"/><text x="${X(v)}" y="${H - 6}" text-anchor="middle">${l}</text>`).join('');
        const marks = [[0.5, 'p50'], [0.99, 'p99'], [0.999, 'p99.9']].map(([q, l], k) => {
            const v = quantile(w, wOver, q);
            if (v === null || !Number.isFinite(v)) return '';
            const x = X(v);
            return `<line x1="${x.toFixed(1)}" x2="${x.toFixed(1)}" y1="${top - 2}" y2="${top + ph}" class="ch-mark"/><text x="${(x + 3).toFixed(1)}" y="${top + 6 + k * 10}" class="ch-mark-label">${l} ${usText(v)}</text>`;
        }).join('');
        return `<svg class="an-chart" viewBox="0 0 ${W} ${H}" role="img" aria-label="Latency distribution">
            <line x1="${left}" x2="${W - right}" y1="${top + ph}" y2="${top + ph}" class="ch-axis"/>
            <g class="ch-bars">${bars}</g><path d="${step}" class="ch-step"/>${marks}${ticks}</svg>`;
    }

    // Lines over time. series: [{t, ...}], lines: [{key, cls, label}], log: y axis.
    function seriesSvg(series, lines, { log = false, unit = '', height = 112, format = (v) => fmt(v) } = {}) {
        const H = height, top = 10, bottom = 18, left = 2, right = 58;
        const now = Date.now();
        const t0 = Math.max(st.startedAt, now - HISTORY_MS);
        const pts = series.filter((p) => p.t >= t0);
        const vals = [];
        for (const p of pts) for (const l of lines) { const v = p[l.key]; if (v !== null && v !== undefined && Number.isFinite(v) && (!log || v > 0)) vals.push(v); }
        if (pts.length < 2 || !vals.length) {
            const wait = Math.max(0, Math.ceil((LAT_POLL_MS * 2 - (now - st.startedAt)) / 1000));
            return `<svg class="an-chart" viewBox="0 0 ${W} ${H}"><text x="${W / 2}" y="${H / 2}" text-anchor="middle" class="ch-empty">${wait ? `Collecting · first points in ${wait} s` : 'No data yet'}</text></svg>`;
        }
        let lo = Math.min(...vals), hi = Math.max(...vals);
        if (log) { lo = 10 ** Math.floor(Math.log10(lo)); hi = 10 ** Math.ceil(Math.log10(hi)); if (hi <= lo) hi = lo * 10; }
        else { lo = 0; hi = hi > 0 ? hi * 1.15 : 1; }
        const ph = H - top - bottom, pw = W - left - right;
        const X = (t) => left + ((t - t0) / Math.max(1, now - t0)) * pw;
        const Y = (v) => top + ph - (log ? (Math.log10(v) - Math.log10(lo)) / (Math.log10(hi) - Math.log10(lo)) : (v - lo) / (hi - lo)) * ph;
        let grid = '';
        if (log) {
            for (let v = lo; v <= hi * 1.001; v *= 10) grid += `<line x1="${left}" x2="${left + pw}" y1="${Y(v).toFixed(1)}" y2="${Y(v).toFixed(1)}" class="ch-grid"/><text x="${left + pw + 4}" y="${(Y(v) + 3).toFixed(1)}" class="ch-y">${usText(v)}</text>`;
        } else {
            for (const f of [0, 0.5, 1]) { const v = lo + f * (hi / 1.15); grid += `<line x1="${left}" x2="${left + pw}" y1="${Y(v).toFixed(1)}" y2="${Y(v).toFixed(1)}" class="ch-grid"/><text x="${left + pw + 4}" y="${(Y(v) + 3).toFixed(1)}" class="ch-y">${format(v)}</text>`; }
        }
        // minute ticks back from now
        const span = now - t0;
        const stepMin = span > 10 * 60e3 ? 5 : span > 4 * 60e3 ? 2 : 1;
        let xt = '';
        for (let m = 0; m * 60e3 <= span; m += stepMin) {
            const x = X(now - m * 60e3);
            xt += `<line x1="${x.toFixed(1)}" x2="${x.toFixed(1)}" y1="${top + ph}" y2="${top + ph + 3}" class="ch-tick"/><text x="${x.toFixed(1)}" y="${H - 5}" text-anchor="middle">${m ? `−${m}′` : 'now'}</text>`;
        }
        const paths = lines.map((l) => {
            let d = '';
            let pen = false;
            for (const p of pts) {
                const v = p[l.key];
                if (v === null || v === undefined || !Number.isFinite(v) || (log && v <= 0)) { pen = false; continue; }
                d += `${pen ? 'L' : 'M'}${X(p.t).toFixed(1)} ${Y(Math.min(hi, Math.max(lo, v))).toFixed(1)} `;
                pen = true;
            }
            return `<path d="${d}" class="${l.cls}"/>`;
        }).join('');
        const lastP = pts[pts.length - 1];
        const labels = lines.map((l, k) => {
            const v = lastP[l.key];
            if (v === null || v === undefined) return '';
            return `<text x="${left + pw - 2}" y="${(top + 8 + k * 10).toFixed(1)}" text-anchor="end" class="ch-legend ${l.cls}-t">${l.label} ${log ? usText(v) : format(v)}${unit}</text>`;
        }).join('');
        return `<svg class="an-chart" viewBox="0 0 ${W} ${H}">${grid}<line x1="${left}" x2="${left + pw}" y1="${top + ph}" y2="${top + ph}" class="ch-axis"/>${xt}${paths}${labels}</svg>`;
    }

    // Rates per interval from cumulative samples, binned to `binMs`.
    function rates(samples, keys, binMs = 5000) {
        const out = [];
        let prev = null;
        for (const s of samples) {
            if (!prev) { prev = s; continue; }
            if (s.t - prev.t < binMs * 0.9) continue;
            const dt = (s.t - prev.t) / 1000;
            const p = { t: s.t };
            for (const k of keys) p[k] = s[k] >= prev[k] ? (s[k] - prev[k]) / dt : null;
            out.push(p);
            prev = s;
        }
        return out;
    }

    // Range by bearing from the receiver: the furthest position seen in each
    // 10° sector since the page opened, and where every aircraft is now.
    function coverageSvg(list) {
        const S = W, c = S / 2, R = c - 26;
        const [rlat, rlon] = st.host.receiver;
        let maxNm = Math.max(...st.coverage);
        const now = [];
        for (const a of list) {
            if (a.lat === null || a.lon === null || a.lat === undefined) continue;
            const { nm, brg } = haversine(rlat, rlon, a.lat, a.lon);
            now.push({ nm, brg, a });
            maxNm = Math.max(maxNm, nm);
        }
        if (!maxNm) return '<p class="muted">No positions yet.</p>';
        const step = [10, 25, 50, 100, 200, 500].find((s) => maxNm / s <= 4) || 1000;
        const outer = Math.ceil(maxNm / step) * step;
        const P = (nm, brg) => { const r = (nm / outer) * R; const t = (brg * Math.PI) / 180; return [c + r * Math.sin(t), c - r * Math.cos(t)]; };
        let rings = '';
        for (let nm = step; nm <= outer + 1e-6; nm += step) {
            rings += `<circle cx="${c}" cy="${c}" r="${((nm / outer) * R).toFixed(1)}" class="cv-ring"/><text x="${c + 3}" y="${(c - (nm / outer) * R - 3).toFixed(1)}" class="cv-ring-l">${nm} NM</text>`;
        }
        let spokes = '';
        for (let b = 0; b < 360; b += 30) { const [x, y] = P(outer, b); spokes += `<line x1="${c}" y1="${c}" x2="${x.toFixed(1)}" y2="${y.toFixed(1)}" class="cv-spoke"/>`; }
        const card = [['N', 0], ['E', 90], ['S', 180], ['W', 270]].map(([l, b]) => { const [x, y] = P(outer * 1.09, b); return `<text x="${x.toFixed(1)}" y="${(y + 4).toFixed(1)}" text-anchor="middle" class="cv-card">${l}</text>`; }).join('');
        let poly = '';
        for (let s = 0; s < 36; s++) {
            const nm = st.coverage[s];
            const [xa, ya] = P(nm, s * 10);
            const [xb, yb] = P(nm, s * 10 + 10);
            poly += `${s ? 'L' : 'M'}${xa.toFixed(1)} ${ya.toFixed(1)} A${((nm / outer) * R).toFixed(1)} ${((nm / outer) * R).toFixed(1)} 0 0 1 ${xb.toFixed(1)} ${yb.toFixed(1)} `;
        }
        const dots = now.map(({ nm, brg, a }) => { const [x, y] = P(nm, brg); return `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="1.9" class="cv-dot" data-band="${band(a)}"/>`; }).join('');
        return `<svg class="an-chart an-coverage" viewBox="0 0 ${S} ${S}" role="img" aria-label="Range by bearing">
            ${rings}${spokes}<path d="${poly}Z" class="cv-area"/>${dots}
            <circle cx="${c}" cy="${c}" r="2.5" class="cv-rx"/>${card}</svg>`;
    }

    function barsSvg(bins, { label = (b) => b.label, cls = () => '' } = {}) {
        const H = 74, top = 6, bottom = 16;
        const max = Math.max(1, ...bins.map((b) => b.n));
        const bw = W / bins.length;
        return `<svg class="an-chart" viewBox="0 0 ${W} ${H}">${bins.map((b, i) => {
            const h = (b.n / max) * (H - top - bottom);
            return `<rect x="${(i * bw + 1).toFixed(1)}" y="${(H - bottom - h).toFixed(1)}" width="${(bw - 2).toFixed(1)}" height="${h.toFixed(1)}" class="${cls(b)}"/>${b.n ? `<text x="${(i * bw + bw / 2).toFixed(1)}" y="${(H - bottom - h - 2).toFixed(1)}" text-anchor="middle" class="ch-val">${b.n}</text>` : ''}<text x="${(i * bw + bw / 2).toFixed(1)}" y="${H - 4}" text-anchor="middle">${label(b, i)}</text>`;
        }).join('')}<line x1="0" x2="${W}" y1="${H - bottom}" y2="${H - bottom}" class="ch-axis"/></svg>`;
    }

    // ---- sections -------------------------------------------------------------

    const fig = (v, unit, label, cls = '') => `<div class="${cls}"><b>${v}</b>${unit ? `<em>${unit}</em>` : ''}<label>${label}</label></div>`;

    function pageNumbers() {
        const p = st.host?.page?.();
        if (!p) return null;
        const r = rates(st.pageSamples.slice(-12), ['frames', 'bytes', 'updates', 'renders'], 1000);
        const avg = (k) => (r.length ? sum(r.map((x) => x[k] ?? 0)) / r.length : null);
        const times = Array.from(p.renderTimes || []).filter((x) => x > 0).sort((a, b) => a - b);
        const q = (f) => (times.length ? times[Math.min(times.length - 1, Math.floor(f * times.length))] : null);
        return { p, fps: avg('frames'), bps: avg('bytes'), ups: avg('updates'), rps: avg('renders'), r50: q(0.5), r99: q(0.99), rmax: times.length ? times[times.length - 1] : null };
    }

    function systemTab() {
        const s = st.stats;
        if (!s) {
            return `<p class="an-lede">No gateway here.</p><p class="muted">This page is served by the Fall 2025 server, which polls a database every 2 s. Feed, latency and integrity figures come from the WebSocket gateway; start the map through it to see them.</p>${benchmark()}`;
        }
        const sub = s.subscriber || {};
        const fan = s.fanout || {};
        const lat = s.latency_us || {};
        const status = s.status || {};
        const thr = rates(st.statsSamples, ['applied', 'frames', 'bytes'], 5000);
        const lastThr = thr[thr.length - 1];
        const pg = pageNumbers();
        const latLast = st.lat.series[st.lat.series.length - 1];
        const caughtUp = sub.expected === sub.known_next && !status.stale;
        const recovered = (sub.gaps ?? 0) > 0;

        // the lede: one sentence, from the counters
        const lede = `${fmt(sub.applied)} messages applied in sequence${recovered ? `; ${fmt(sub.gaps)} gaps, recovered by retransmission or snapshot` : ', with no gaps'}. Median publish → WebSocket write ${usText(lat.feed_to_ws?.p50_us)}${latLast?.w50 ? `, ${usText(latLast.w50)} over the last 5 s` : ''}.`;

        const link = (v, sub1, sub2) => `<li class="pipe-link"><b>${v}</b><span>${sub1}</span>${sub2 ? `<em>${sub2}</em>` : ''}</li>`;
        const stage = (name, what, note) => `<li class="pipe-stage"><b>${name}</b><span>${what}</span>${note ? `<em>${note}</em>` : ''}</li>`;
        const pipe = `<ol class="an-pipe">
            ${stage('Publisher', 'Sequenced UDP multicast feed, retransmit and snapshot servers', `session ${esc(status.session ?? '—')}`)}
            ${link(usText(lat.feed_to_gateway?.p50_us), `median, publish → applied to the gateway's book · p99 ${usText(lat.feed_to_gateway?.p99_us)}`, lastThr ? `${fmt(lastThr.applied)} msg/s` : '')}
            ${stage('Gateway', `Book of ${fmt(status.tracks)} tracks · gap recovery · per-client conflation`, `seq ${fmt(status.as_of_seq)}`)}
            ${link(usText(lat.feed_to_ws?.p50_us), `median, publish → written to a browser's socket · p99 ${usText(lat.feed_to_ws?.p99_us)}`, lastThr ? `${fmt(status.clients)} client${status.clients === 1 ? '' : 's'} · ${fmt(lastThr.frames)} frames/s` : '')}
            ${pg ? stage('This page', `WebSocket, flow control window 8 · ${pg.fps === null ? '—' : fmt(pg.fps)} frames/s · ${pg.bps === null ? '—' : bytes(pg.bps)}/s`, `${fmt(pg.ups)} updates/s`) : stage('This page', 'Polls the gateway\'s REST API every 2 s', '')}
            ${pg ? link(pg.r50 === null ? '—' : `${fmt(pg.r50, 1)} ms`, `median redraw of the map · p99 ${pg.r99 === null ? '—' : `${fmt(pg.r99, 1)} ms`}`, pg.rps === null ? '' : `${fmt(pg.rps, 1)} redraws/s`) : ''}
            ${pg ? stage('Map', 'Redrawn at most every 100 ms, from the latest table', '') : ''}
        </ol>`;

        // latency distribution, last minute or since start
        let dist = '<p class="muted">Collecting…</p>';
        let distCaption = '';
        if (st.lat.available === false) {
            dist = '<p class="muted">This gateway does not serve /gateway/latency (an older build).</p>';
        } else if (st.lat.snaps.length) {
            const last = st.lat.snaps[st.lat.snaps.length - 1];
            let base = null;
            if (st.histWindow === 'minute') {
                base = st.lat.snaps[0];
                for (const sn of st.lat.snaps) if (sn.t <= last.t - 60_000) base = sn;
                if (base === last) base = null;
            }
            const w = diff(last.w, base?.w), g = diff(last.g, base?.g);
            const wOver = last.wOver - (base?.wOver ?? 0), gOver = last.gOver - (base?.gOver ?? 0);
            dist = histogramSvg(w, wOver, g, gOver);
            distCaption = `${fmt(sum(w) + wOver)} socket writes${base ? ` from ${clock(base.t)} to ${clock(last.t)}` : ' since the gateway started'}`;
        }

        const ledger = (rows) => `<dl class="an-ledger">${rows.map(([k, v, cls]) => `<dt>${k}</dt><dd class="${cls || ''}">${v}</dd>`).join('')}</dl>`;
        const clk = s.clock || {};
        return `
            <p class="an-lede">${lede}</p>
            <h3>Pipeline <span>live, measured at the gateway</span></h3>
            ${pipe}
            <h3>Latency distribution <span class="an-switch" data-switch="hist"><button data-v="minute" class="${st.histWindow === 'minute' ? 'on' : ''}">Last minute</button><button data-v="all" class="${st.histWindow === 'all' ? 'on' : ''}">Since start</button></span></h3>
            ${dist}
            <p class="an-caption"><i class="key key-bar"></i>publish → WebSocket write <i class="key key-step"></i>publish → gateway book${distCaption ? ` · ${distCaption}` : ''}</p>
            <h3>Latency over time <span>per 5 s interval</span></h3>
            ${st.lat.available === false ? '<p class="muted">Needs /gateway/latency.</p>' : seriesSvg(st.lat.series, [{ key: 'w50', cls: 'ln-a', label: 'p50' }, { key: 'w99', cls: 'ln-b', label: 'p99' }], { log: true })}
            <p class="an-caption">Publish → WebSocket write, from the difference of two histogram readings: each point is that interval alone, not the running total.</p>
            <h3>Throughput <span>per 5 s</span></h3>
            ${seriesSvg(thr, [{ key: 'applied', cls: 'ln-a', label: 'feed' }, { key: 'frames', cls: 'ln-b', label: 'frames out' }], { unit: '/s', height: 96 })}
            <h3>Integrity</h3>
            <p class="an-verdict ${caughtUp ? '' : 'is-warn'}">${caughtUp ? 'Caught up: no known message is missing.' : status.stale ? 'Recovering: the gateway is waiting for missing messages.' : `Filling a gap: ${fmt((sub.known_next ?? 0) - (sub.expected ?? 0))} messages outstanding.`}</p>
            ${ledger([
                ['Messages applied', fmt(sub.applied)],
                ['Next expected / announced', `${fmt(sub.expected)} / ${fmt(sub.known_next)}`],
                ['Gaps detected', fmt(sub.gaps), sub.gaps ? 'warn' : ''],
                ['Retransmit requests · retries', `${fmt(sub.requests)} · ${fmt(sub.request_retries)}`],
                ['NAKs (too old to retransmit)', fmt(sub.naks)],
                ['Duplicates dropped', fmt(sub.duplicates)],
                ['Arrived out of order, held', fmt(sub.buffered)],
                ['Stale or malformed packets', `${fmt(sub.stale_packets)} · ${fmt(sub.malformed)}`],
                ['Snapshots requested · loaded', `${fmt(sub.snapshot_requests)} · ${fmt(sub.snapshots_loaded)}`],
                ['Session changes · confirmed', `${fmt(sub.session_changes)} · ${fmt(sub.sessions_confirmed)}`],
                ['Datagrams dropped by the test injector', fmt(sub.feed_dropped)],
            ])}
            <h3>Fan-out to browsers</h3>
            ${ledger([
                ['Clients connected · ever', `${fmt(status.clients)} · ${fmt(fan.clients_total)}`],
                ['Frames sent · snapshots', `${fmt(fan.frames)} · ${fmt(fan.snapshot_frames)}`],
                ['Data sent', bytes(fan.bytes)],
                ['Updates conflated', fmt(fan.conflated)],
                ['Flow-control holds', fmt(fan.flow_holds)],
                ['Stuck clients dropped · refused', `${fmt(fan.clients_dropped_stuck)} · ${fmt(fan.clients_rejected)}`],
            ])}
            ${pg ? `<h3>This page</h3>${ledger([
                ['Connected', pg.p.connectedAt ? `${clock(pg.p.connectedAt)} · ${duration(Date.now() - pg.p.connectedAt)}` : '—'],
                ['Frames received', fmt(pg.p.frames)],
                ['Data received', bytes(pg.p.bytes)],
                ['Aircraft updates applied', fmt(pg.p.updates)],
                ['Map redraws · slowest', `${fmt(pg.p.renders)} · ${pg.rmax === null ? '—' : `${fmt(pg.rmax, 1)} ms`}`],
                ['Reconnects', fmt(pg.p.reconnects)],
            ])}` : ''}
            <p class="an-caption an-clock">${clk.calibrated ? `Latency is publish timestamp → gateway clock, calibrated against the publisher's clock: offset ${fmt((clk.offset_ns ?? 0) / 1000, 1)} µs ± ${fmt((clk.uncertainty_ns ?? 0) / 1000, 1)} µs.` : 'The gateway clock is not calibrated against the publisher: latency figures assume a shared clock.'} Measured on this machine, not the benchmark host.</p>
            ${benchmark()}`;
    }

    function benchmark() {
        return `<h3>Before and after <span>benchmark, not live</span></h3>
            <table class="an-bench">
                <thead><tr><th></th><th>Fall 2025</th><th>This system</th></tr></thead>
                <tbody>
                    <tr><th>Frame received → on the map</th><td>1.10–1.59 s</td><td>69–88 ms</td></tr>
                    <tr><th>Transmitted positions shown</th><td>17%</td><td>76–78%</td></tr>
                    <tr><th>Map vs true position</th><td>461–540 m</td><td>66–69 m</td></tr>
                    <tr><th>Receive → track state</th><td>—</td><td>33–67 µs</td></tr>
                </tbody>
            </table>
            <p class="an-caption">Medians at 50–3,000 synthetic aircraft through readsb, same traffic for both; laptop, Docker. From bench/results/legacy-baseline.</p>`;
    }

    function trafficTab() {
        const list = st.host?.getAircraft?.() || [];
        const air = list.filter((a) => !a.on_ground);
        const mean = (arr) => (arr.length ? sum(arr) / arr.length : null);
        const spd = mean(air.map((a) => a.speed).filter((v) => v !== null && v !== undefined));
        const alt = mean(air.map((a) => a.altitude).filter((v) => v !== null && v !== undefined));
        const [rlat, rlon] = st.host.receiver;

        // furthest sector
        let best = 0;
        for (let s = 1; s < 36; s++) if (st.coverage[s] > st.coverage[best]) best = s;

        const bands = [['top', '≥ 30k'], ['high', '20–30k'], ['mid', '10–20k'], ['low', '< 10k'], ['ground', 'Ground']];
        const bandN = Object.fromEntries(bands.map(([b]) => [b, 0]));
        for (const a of list) bandN[band(a)]++;
        const bmax = Math.max(1, ...Object.values(bandN));

        const speedBins = [];
        for (let k = 0; k < 12; k++) speedBins.push({ lo: k * 50, n: 0 });
        for (const a of air) if (a.speed !== null && a.speed !== undefined) speedBins[Math.min(11, Math.floor(a.speed / 50))].n++;

        let climb = 0, level = 0, desc = 0;
        for (const a of air) {
            const v = a.vertical_rate;
            if (v === null || v === undefined || Math.abs(v) <= 300) level++;
            else if (v > 0) climb++;
            else desc++;
        }
        const vt = Math.max(1, climb + level + desc);

        // busiest: updates per second over the last ~30 s, from the page's counters
        let busiest = '';
        if (st.updSnaps.length >= 2) {
            const last = st.updSnaps[st.updSnaps.length - 1];
            let first = st.updSnaps[0];
            for (const sn of st.updSnaps) if (sn.t <= last.t - 30_000) first = sn;
            const dt = (last.t - first.t) / 1000;
            if (dt > 0) {
                const byHex = new Map(list.map((a) => [a.hex, a]));
                const rows = [];
                for (const [icao, n] of last.counts) {
                    const r = (n - (first.counts.get(icao) ?? 0)) / dt;
                    const hex = icao.toString(16).toUpperCase().padStart(6, '0');
                    if (r > 0) rows.push({ hex, r, a: byHex.get(hex) });
                }
                rows.sort((a, b) => b.r - a.r);
                const total = sum(rows.map((x) => x.r));
                busiest = `<h3>Busiest aircraft <span>updates/s, last ${Math.round(dt)} s</span></h3>
                    <ol class="an-rank">${rows.slice(0, 8).map((x) => `<li data-hex="${x.hex}"><b>${esc(x.a?.flight || x.hex)}</b><i style="--w:${((x.r / rows[0].r) * 100).toFixed(1)}%"></i><span>${fmt(x.r, 1)}</span></li>`).join('')}</ol>
                    <p class="an-caption">${fmt(total, 0)} updates/s across ${fmt(rows.length)} aircraft, mean ${fmt(rows.length ? total / rows.length : 0, 2)} per aircraft. Each is one feed message after the gateway's conflation for this page.</p>`;
            }
        }

        const emerg = list.filter((a) => EMERGENCY[a.squawk]);
        const durs = (st.statistics?.flight_durations || []).slice().sort((a, b) => (b.duration_seconds ?? 0) - (a.duration_seconds ?? 0)).slice(0, 6);

        return `
            <div class="an-figures">
                ${fig(fmt(list.length), '', 'Tracked now')}
                ${fig(fmt(st.firstSeen.size), '', `Seen since ${clock(st.startedAt).slice(0, 5)}`)}
                ${fig(fmt(spd), 'kt', 'Mean speed')}
                ${fig(alt === null ? '—' : fmt(alt / 1000, 1), 'k ft', 'Mean altitude')}
            </div>
            <h3>Coverage <span>range by bearing from the receiver</span></h3>
            ${coverageSvg(list)}
            <p class="an-caption"><i class="key key-area"></i>furthest position seen in each 10° sector since this page opened · dots: aircraft now${st.coverage[best] ? ` · furthest ${fmt(st.coverage[best])} NM at ${String(best * 10).padStart(3, '0')}–${String(best * 10 + 10).padStart(3, '0')}°` : ''}. Receiver ${Math.abs(rlat).toFixed(2)}° ${rlat >= 0 ? 'N' : 'S'}, ${Math.abs(rlon).toFixed(2)}° ${rlon >= 0 ? 'E' : 'W'}.</p>
            <h3>Altitude</h3>
            ${bands.map(([b, l]) => `<div class="an-bar" data-band="${b}"><span>${l}</span><i style="--w:${((bandN[b] / bmax) * 100).toFixed(1)}%"></i><b>${bandN[b]}</b></div>`).join('')}
            <h3>Ground speed <span>airborne, kt</span></h3>
            ${barsSvg(speedBins, { label: (b, i) => (i % 2 === 0 ? String(b.lo) : ''), cls: () => 'bar' })}
            <h3>Vertical <span>airborne, ±300 ft/min is level</span></h3>
            <div class="an-split"><i class="up" style="--w:${((climb / vt) * 100).toFixed(1)}%"></i><i class="lv" style="--w:${((level / vt) * 100).toFixed(1)}%"></i><i class="dn" style="--w:${((desc / vt) * 100).toFixed(1)}%"></i></div>
            <div class="an-split-l"><span><b>${climb}</b> climbing</span><span><b>${level}</b> level</span><span><b>${desc}</b> descending</span></div>
            ${busiest}
            <h3>Longest tracked</h3>
            <ol class="an-durations">${durs.map((d) => `<li data-hex="${esc(d.hex)}"><b>${esc(d.flight || d.hex)}</b><span>${duration((d.duration_seconds ?? 0) * 1000)}</span></li>`).join('') || '<li class="muted">None yet</li>'}</ol>
            <h3>Emergency squawks</h3>
            ${emerg.length ? `<ol class="an-durations">${emerg.map((a) => `<li data-hex="${a.hex}" class="is-alert"><b>${esc(a.flight || a.hex)}</b><span>${a.squawk} · ${EMERGENCY[a.squawk]}</span></li>`).join('')}</ol>` : '<p class="muted">None (7500, 7600, 7700).</p>'}`;
    }

    // ---- rendering --------------------------------------------------------------

    function render() {
        if (!st.open || !st.body) return;
        st.lastRender = Date.now();
        const tabs = `<nav class="an-tabs">${[['system', 'System'], ['traffic', 'Traffic']].map(([k, l]) => `<button data-tab="${k}" class="${st.tab === k ? 'on' : ''}">${l}</button>`).join('')}</nav>`;
        st.body.innerHTML = tabs + (st.tab === 'system' ? systemTab() : trafficTab());
    }

    function onClick(e) {
        const t = e.target.closest('[data-tab]');
        if (t) { st.tab = t.dataset.tab; st.body.scrollTop = 0; render(); return; }
        const sw = e.target.closest('[data-switch="hist"] button');
        if (sw) { st.histWindow = sw.dataset.v; render(); return; }
        const row = e.target.closest('[data-hex]');
        if (row && st.host?.onSelect) st.host.onSelect(row.dataset.hex);
    }

    function init(host, body) {
        st.host = host;
        st.body = body;
        body.addEventListener('click', onClick);
        setInterval(samplePage, 1000);
        setInterval(sampleTraffic, TRAFFIC_MS);
        setInterval(pollLatency, LAT_POLL_MS);
        setInterval(() => { if (st.open) render(); }, 2000);
        pollLatency();
        setTimeout(sampleTraffic, 1500);
    }

    function show(open) {
        st.open = open;
        if (open) render();
    }

    function setStatistics(data) {
        st.statistics = data;
    }

    return { init, show, ingestStats, setStatistics, render, _st: st };
})();
