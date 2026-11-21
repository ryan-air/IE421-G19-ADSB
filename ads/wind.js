// ---------------------------------------------------------------------------
// Wind layer: streamlines of the wind at one flight level, as on a weather
// chart: long, evenly spaced lines that follow the flow across the view, with
// small arrowheads that drift slowly downstream.
//
// Data: Open-Meteo's current model wind (keyless, CC BY 4.0) on a grid of
// points over the view, fetched again when the view leaves the grid or every
// 15 minutes. It is weather-model output, not something this receiver
// measured, and the legend says so. Line weight and arrow drift grow with
// wind speed (relative to the strongest wind in view); direction is the wind's.
// Without network access the layer reports "unavailable" and draws nothing.
// ---------------------------------------------------------------------------
const AtlasWind = (() => {
    const LEVELS = {
        '10m': { label: 'Surface', short: 'SFC' },
        '850hPa': { label: 'FL050', short: 'FL050', hpa: 850 },
        '500hPa': { label: 'FL180', short: 'FL180', hpa: 500 },
        '250hPa': { label: 'FL340', short: 'FL340', hpa: 250 },
    };
    const GRID_X = 12, GRID_Y = 8;
    const REFRESH_MS = 15 * 60 * 1000;
    const FPS = 30;

    const st = {
        map: null, pane: null, canvas: null, ctx: null, legend: null,
        enabled: true, level: '10m',
        grid: null, fetchedAt: 0, fetching: false, error: null,
        lines: [], w: 0, h: 0, dpr: 1, phase: 0,
        moving: false, raf: 0, last: 0,
        reduced: window.matchMedia?.('(prefers-reduced-motion: reduce)').matches,
    };

    // ---- data ---------------------------------------------------------------

    async function fetchGrid() {
        if (st.fetching || !st.enabled) return;
        st.fetching = true;
        const b = st.map.getBounds().pad(0.25);
        const s = Math.max(-85, b.getSouth()), n = Math.min(85, b.getNorth());
        const w = b.getWest(), e = b.getEast();
        const lats = [], lons = [];
        for (let j = 0; j < GRID_Y; j++) {
            for (let i = 0; i < GRID_X; i++) {
                lats.push((s + ((n - s) * j) / (GRID_Y - 1)).toFixed(3));
                lons.push((((w + ((e - w) * i) / (GRID_X - 1)) + 540) % 360 - 180).toFixed(3));
            }
        }
        const lv = st.level;
        const url = `https://api.open-meteo.com/v1/forecast?latitude=${lats.join(',')}&longitude=${lons.join(',')}`
            + `&current=wind_speed_${lv},wind_direction_${lv}&wind_speed_unit=kn&timeformat=unixtime`;
        try {
            const r = await fetch(url);
            if (!r.ok) throw new Error(`HTTP ${r.status}`);
            const d = await r.json();
            const rows = Array.isArray(d) ? d : [d];
            if (rows.length !== GRID_X * GRID_Y) throw new Error('unexpected reply');
            const u = new Float32Array(rows.length), v = new Float32Array(rows.length);
            let max = 0, time = 0;
            rows.forEach((p, k) => {
                const spd = p.current?.[`wind_speed_${lv}`];
                const dir = p.current?.[`wind_direction_${lv}`];
                if (spd === null || spd === undefined || dir === null || dir === undefined) return;
                const rad = (dir * Math.PI) / 180; // direction the wind comes from
                u[k] = -spd * Math.sin(rad); // towards east
                v[k] = -spd * Math.cos(rad); // towards north
                max = Math.max(max, spd);
                time = Math.max(time, p.current.time || 0);
            });
            st.grid = { s, n, w, e, u, v, max, time: time * 1000, level: lv };
            st.fetchedAt = Date.now();
            st.error = null;
            seed();
        } catch (err) {
            st.error = err.message || 'unavailable';
        } finally {
            st.fetching = false;
            updateLegend();
        }
    }

    function needsFetch() {
        if (!st.enabled) return false;
        const g = st.grid;
        if (!g || g.level !== st.level || Date.now() - st.fetchedAt > REFRESH_MS) return true;
        const b = st.map.getBounds();
        return b.getSouth() < g.s || b.getNorth() > g.n || b.getWest() < g.w || b.getEast() > g.e;
    }

    // Wind (kt, east/north) at a position, null outside the grid. Bicubic
    // (Catmull-Rom) rather than bilinear: bilinear bends streamlines at every
    // grid-cell edge.
    function windAt(lat, lon) {
        const g = st.grid;
        if (!g) return null;
        const fx = ((lon - g.w) / (g.e - g.w)) * (GRID_X - 1);
        const fy = ((lat - g.s) / (g.n - g.s)) * (GRID_Y - 1);
        if (fx < 0 || fy < 0 || fx > GRID_X - 1 || fy > GRID_Y - 1) return null;
        const i = Math.min(GRID_X - 2, Math.floor(fx)), j = Math.min(GRID_Y - 2, Math.floor(fy));
        const tx = fx - i, ty = fy - j;
        const cr = (p0, p1, p2, p3, t) => p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
        const at = (a, x, y) => a[Math.max(0, Math.min(GRID_Y - 1, y)) * GRID_X + Math.max(0, Math.min(GRID_X - 1, x))];
        const bicubic = (a) => {
            const r = [];
            for (let m = -1; m <= 2; m++) r.push(cr(at(a, i - 1, j + m), at(a, i, j + m), at(a, i + 1, j + m), at(a, i + 2, j + m), tx));
            return cr(r[0], r[1], r[2], r[3], ty);
        };
        return [bicubic(g.u), bicubic(g.v)];
    }

    // ---- drawing --------------------------------------------------------------

    function resize() {
        const size = st.map.getSize();
        st.dpr = window.devicePixelRatio || 1;
        st.w = size.x; st.h = size.y;
        st.canvas.width = Math.round(st.w * st.dpr);
        st.canvas.height = Math.round(st.h * st.dpr);
        st.canvas.style.width = `${st.w}px`;
        st.canvas.style.height = `${st.h}px`;
        place();
    }

    // keep the canvas over the viewport (the pane moves with the map)
    function place() {
        L.DomUtil.setPosition(st.canvas, st.map.containerPointToLayerPoint([0, 0]));
    }

    // ---- streamlines ---------------------------------------------------------
    //
    // Evenly spaced streamlines (after Jobard and Lefer, 1997): seeds on a
    // grid; each line is traced both ways through the field, in screen space,
    // until it leaves the view or comes within DTEST of a line already drawn.
    // The field is sampled once per view on a coarse pixel grid, so tracing
    // costs no map projections.

    const DSEP = 46;        // px between neighbouring lines
    const DTEST = DSEP * 0.5;
    const STEP = 2.5;       // px per integration step
    const MAX_STEPS = 900;
    const CELL = 16;        // px, field sampling
    const ARROW_GAP = 150;  // px between arrowheads along a line

    let field = null; // {nx, ny, u, v, s} in px space, v pointing down

    function sampleField() {
        const nx = Math.ceil(st.w / CELL) + 2, ny = Math.ceil(st.h / CELL) + 2;
        const u = new Float32Array(nx * ny), v = new Float32Array(nx * ny), sp = new Float32Array(nx * ny);
        const ok = new Uint8Array(nx * ny);
        for (let j = 0; j < ny; j++) {
            for (let i = 0; i < nx; i++) {
                const ll = st.map.containerPointToLatLng([(i - 1) * CELL, (j - 1) * CELL]);
                const w = windAt(ll.lat, ll.lng);
                const k = j * nx + i;
                if (!w) continue;
                u[k] = w[0]; v[k] = -w[1]; sp[k] = Math.hypot(w[0], w[1]); ok[k] = 1;
            }
        }
        field = { nx, ny, u, v, sp, ok };
    }

    // wind in px space (east, down) and speed at a screen point, or null
    function fieldAt(x, y) {
        const f = field;
        const fx = x / CELL + 1, fy = y / CELL + 1;
        const i = Math.floor(fx), j = Math.floor(fy);
        if (i < 0 || j < 0 || i >= f.nx - 1 || j >= f.ny - 1) return null;
        const k = j * f.nx + i;
        if (!f.ok[k] || !f.ok[k + 1] || !f.ok[k + f.nx] || !f.ok[k + f.nx + 1]) return null;
        const tx = fx - i, ty = fy - j;
        const l = (a) => (a[k] * (1 - tx) + a[k + 1] * tx) * (1 - ty) + (a[k + f.nx] * (1 - tx) + a[k + f.nx + 1] * tx) * ty;
        return [l(f.u), l(f.v), l(f.sp)];
    }

    // occupancy grid of accepted line points, cell = DTEST
    let occ = null;
    function occInit() {
        occ = { nx: Math.ceil(st.w / DTEST) + 1, ny: Math.ceil(st.h / DTEST) + 1, cells: new Map() };
    }
    function occAdd(x, y, line) {
        const key = Math.floor(y / DTEST) * occ.nx + Math.floor(x / DTEST);
        let c = occ.cells.get(key);
        if (!c) occ.cells.set(key, (c = []));
        c.push(x, y, line);
    }
    function occNear(x, y, d, self = -1) {
        const cx = Math.floor(x / DTEST), cy = Math.floor(y / DTEST);
        const r = Math.ceil(d / DTEST);
        const d2 = d * d;
        for (let j = cy - r; j <= cy + r; j++) {
            for (let i = cx - r; i <= cx + r; i++) {
                const c = occ.cells.get(j * occ.nx + i);
                if (!c) continue;
                for (let q = 0; q < c.length; q += 3) {
                    if (c[q + 2] === self) continue;
                    const dx = c[q] - x, dy = c[q + 1] - y;
                    if (dx * dx + dy * dy < d2) return true;
                }
            }
        }
        return false;
    }

    // trace from (x, y) in direction dir (+1 downstream, -1 upstream)
    function trace(x, y, dir, id) {
        const pts = [];
        for (let n = 0; n < MAX_STEPS; n++) {
            const a = fieldAt(x, y);
            if (!a || a[2] < 0.5) break;
            // midpoint (RK2) on the unit direction: lines follow the flow at an even pace
            const hx = x + (dir * STEP * 0.5 * a[0]) / a[2], hy = y + (dir * STEP * 0.5 * a[1]) / a[2];
            const b = fieldAt(hx, hy);
            if (!b || b[2] < 0.5) break;
            x += (dir * STEP * b[0]) / b[2];
            y += (dir * STEP * b[1]) / b[2];
            if (x < -4 || y < -4 || x > st.w + 4 || y > st.h + 4) break;
            if (occNear(x, y, DTEST, id)) break;
            // closed loop back onto itself
            if (n > 40 && pts.length > 40) {
                const p0 = pts[0];
                if ((p0[0] - x) ** 2 + (p0[1] - y) ** 2 < STEP * STEP * 4) break;
            }
            pts.push([x, y, b[2]]);
        }
        return pts;
    }

    function seed() {
        st.lines = [];
        clear();
        if (!st.grid || !st.w) return;
        const t0 = performance.now();
        sampleField();
        occInit();
        // seeds on a grid, visited from the centre outwards so the middle of the view is even
        const seeds = [];
        for (let y = DSEP / 2; y < st.h; y += DSEP) for (let x = DSEP / 2; x < st.w; x += DSEP) seeds.push([x, y]);
        const cx = st.w / 2, cy = st.h / 2;
        seeds.sort((a, b) => (a[0] - cx) ** 2 + (a[1] - cy) ** 2 - ((b[0] - cx) ** 2 + (b[1] - cy) ** 2));
        let id = 0;
        for (const [sx, sy] of seeds) {
            if (occNear(sx, sy, DSEP)) continue;
            const a = fieldAt(sx, sy);
            if (!a) continue;
            const back = trace(sx, sy, -1, id).reverse();
            const fwd = trace(sx, sy, 1, id);
            const pts = [...back, [sx, sy, a[2]], ...fwd];
            if (pts.length * STEP < 60) continue; // too short to read
            for (const p of pts) occAdd(p[0], p[1], id);
            let len = 0;
            const cum = [0];
            for (let k = 1; k < pts.length; k++) { len += Math.hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]); cum.push(len); }
            const mean = pts.reduce((m, p) => m + p[2], 0) / pts.length;
            st.lines.push({ pts, cum, len, mean, off: (id * 37) % ARROW_GAP });
            id++;
        }
        st.buildMs = performance.now() - t0;
        draw();
    }

    function clear() {
        st.ctx.setTransform(1, 0, 0, 1, 0, 0);
        st.ctx.clearRect(0, 0, st.canvas.width, st.canvas.height);
    }

    const rel = (spd) => Math.min(1, spd / Math.max(25, st.grid?.max || 60));

    // point and direction at distance d along a line
    function along(line, d) {
        const { pts, cum } = line;
        let lo = 0, hi = cum.length - 1;
        while (hi - lo > 1) { const m = (lo + hi) >> 1; if (cum[m] < d) lo = m; else hi = m; }
        const t = (d - cum[lo]) / Math.max(1e-6, cum[hi] - cum[lo]);
        const a = pts[lo], b = pts[hi];
        return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, Math.atan2(b[1] - a[1], b[0] - a[0]), a[2]];
    }

    function draw() {
        const t0 = performance.now();
        const ctx = st.ctx;
        clear();
        ctx.setTransform(st.dpr, 0, 0, st.dpr, 0, 0);
        ctx.lineCap = 'round';
        ctx.lineJoin = 'round';
        for (const line of st.lines) {
            const r = rel(line.mean);
            const { pts } = line;
            // the line, faded in at both ends
            const fadeLen = Math.min(40, line.len / 4);
            const grad = ctx.createLinearGradient(pts[0][0], pts[0][1], pts[pts.length - 1][0], pts[pts.length - 1][1]);
            const a = 0.16 + 0.2 * r;
            const f = Math.min(0.45, fadeLen / Math.max(1, Math.hypot(pts[pts.length - 1][0] - pts[0][0], pts[pts.length - 1][1] - pts[0][1])));
            grad.addColorStop(0, 'rgba(235, 229, 214, 0)');
            grad.addColorStop(f, `rgba(235, 229, 214, ${a.toFixed(3)})`);
            grad.addColorStop(1 - f, `rgba(235, 229, 214, ${a.toFixed(3)})`);
            grad.addColorStop(1, 'rgba(235, 229, 214, 0)');
            ctx.strokeStyle = grad;
            ctx.lineWidth = 0.8 + 0.6 * r;
            ctx.beginPath();
            ctx.moveTo(pts[0][0], pts[0][1]);
            for (let k = 1; k < pts.length; k++) ctx.lineTo(pts[k][0], pts[k][1]);
            ctx.stroke();
            // arrowheads, drifting downstream with the phase
            ctx.strokeStyle = `rgba(235, 229, 214, ${(0.28 + 0.3 * r).toFixed(3)})`;
            ctx.lineWidth = 1;
            const drift = (st.phase * (0.35 + 0.65 * r)) % ARROW_GAP;
            for (let d = line.off + drift + 20; d < line.len - 16; d += ARROW_GAP) {
                const [x, y, ang] = along(line, d);
                const s = 4.5;
                ctx.beginPath();
                ctx.moveTo(x - s * Math.cos(ang - 0.5), y - s * Math.sin(ang - 0.5));
                ctx.lineTo(x, y);
                ctx.lineTo(x - s * Math.cos(ang + 0.5), y - s * Math.sin(ang + 0.5));
                ctx.stroke();
            }
        }
        st.frameMs = performance.now() - t0;
    }

    function frame(t) {
        st.raf = 0;
        if (!st.enabled || !st.grid || st.moving || document.hidden || st.reduced) return;
        if (t - st.last < 1000 / FPS - 2) { st.raf = requestAnimationFrame(frame); return; }
        const dt = Math.min(0.1, (t - (st.last || t)) / 1000) || 1 / FPS;
        st.last = t;
        st.phase += dt * 22; // px/s of arrow drift at the strongest wind
        draw();
        st.raf = requestAnimationFrame(frame);
    }

    function start() {
        if (!st.raf && st.enabled && !st.reduced) { st.last = 0; st.raf = requestAnimationFrame(frame); }
    }

    // ---- legend and controls -------------------------------------------------

    function updateLegend() {
        if (!st.legend) return;
        const lv = LEVELS[st.level];
        if (!st.enabled) { st.legend.hidden = true; return; }
        st.legend.hidden = false;
        if (st.error && !st.grid) { st.legend.innerHTML = `<b>Wind</b> ${lv.label} · unavailable`; return; }
        if (!st.grid) { st.legend.innerHTML = `<b>Wind</b> ${lv.label} · loading`; return; }
        const t = new Date(st.grid.time);
        const hh = String(t.getUTCHours()).padStart(2, '0'), mm = String(t.getUTCMinutes()).padStart(2, '0');
        st.legend.innerHTML = `<b>Wind</b> ${lv.label}${lv.hpa ? ` <span>${lv.hpa} hPa</span>` : ''} · up to ${Math.round(st.grid.max)} kt · model ${hh}${mm}Z <span>· Open-Meteo</span>`;
        st.legend.title = 'Current weather-model wind from Open-Meteo, interpolated over the view. Not measured by this receiver. Streak speed is scaled for legibility.';
    }

    function setEnabled(on) {
        st.enabled = on;
        st.canvas.style.display = on ? '' : 'none';
        updateLegend();
        if (on) {
            if (needsFetch()) fetchGrid().then(start); else { seed(); start(); }
        } else {
            cancelAnimationFrame(st.raf); st.raf = 0;
            clear();
        }
        const attr = st.map.attributionControl;
        if (attr) on ? attr.addAttribution('Wind: <a href="https://open-meteo.com/">Open-Meteo</a>') : attr.removeAttribution('Wind: <a href="https://open-meteo.com/">Open-Meteo</a>');
    }

    function setLevel(level) {
        if (!LEVELS[level] || level === st.level) return;
        st.level = level;
        st.grid = null;
        clear();
        updateLegend();
        fetchGrid().then(start);
    }

    function init(map, { legend } = {}) {
        st.map = map;
        st.legend = legend || null;
        st.pane = map.createPane('windPane');
        st.pane.style.zIndex = 390; // over the tiles (200), under trails (405) and aircraft (600)
        st.pane.style.pointerEvents = 'none';
        st.canvas = L.DomUtil.create('canvas', 'wind-canvas', st.pane);
        st.ctx = st.canvas.getContext('2d');
        resize();
        map.on('movestart zoomstart', () => {
            st.moving = true;
            st.canvas.style.opacity = '0';
        });
        map.on('moveend zoomend', () => {
            if (!st.moving) return;
            st.moving = false;
            place();
            st.canvas.style.opacity = '';
            if (!st.enabled) return;
            if (needsFetch()) fetchGrid().then(start);
            else { seed(); start(); }
        });
        map.on('resize', () => { resize(); seed(); start(); });
        document.addEventListener('visibilitychange', start);
        setInterval(() => { if (st.enabled && Date.now() - st.fetchedAt > REFRESH_MS) fetchGrid(); }, 60_000);
        map.attributionControl?.addAttribution('Wind: <a href="https://open-meteo.com/">Open-Meteo</a>');
        updateLegend();
        fetchGrid().then(start);
    }

    return { init, setEnabled, setLevel, LEVELS, _st: st };
})();
