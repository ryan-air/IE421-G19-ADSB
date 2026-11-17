#!/usr/bin/env python3
"""Analysis for the Fall 2025 baseline harness.

  analyze.py scenario DIR   metrics for one run -> DIR/metrics.json (+ summary on stdout)
  analyze.py report OUT     markdown report over all OUT/n*/metrics.json

Every position the old system shows (readsb SBS output, database row, API
response) is matched to the transmitted frame it came from, using the
generator's ground truth: per aircraft, the truth point nearest the shown
position. Frames are >= 0.4 s apart (~100 m at airliner speed) and CPR error is
~5 m, so the match is unambiguous; anything farther than MATCH_M is counted as
unmatched rather than guessed.
"""

import csv
import json
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

MATCH_M = 50.0          # max distance between a shown position and its source frame
MATCH_WINDOW_S = 60.0   # source frame must have been sent within this long before it was shown.
                        # Turning aircraft fly over their own earlier positions (a full
                        # circle takes >= 120 s), so an unbounded search can match a frame
                        # from a previous lap and report a latency of minutes.
WARMUP_S = 20.0         # ignore the first seconds of each run (tracks still forming)
POLL_S = 2.0            # the Fall 2025 map polls /api/aircraft every 2 s (ads/app.js)
RX_LATENCY_NS = 1_650_000  # generator: tx -> record rx (1.5 ms fixed + 0.15 ms mean jitter)
EARTH_M_PER_DEG = 111_320.0


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

class Truth:
    """Per-aircraft ground truth, with emit times mapped to wall clock."""

    def __init__(self, path, feed):
        rows = defaultdict(list)
        with open(path) as f:
            for r in csv.DictReader(f):
                rows[r["icao"]].append((int(r["tx_ns"]), float(r["lat"]), float(r["lon"]),
                                        int(r["alt_ft"]), r["on_ground"] == "1"))
        self.t0 = feed["t0_wall_ns"]
        self.first_rx = feed["first_rx_ns"]
        self.speed = feed["speed"]
        self.by_hex = {}
        for hx, pts in rows.items():
            pts.sort()
            tx = np.array([p[0] for p in pts], dtype=np.int64)
            self.by_hex[hx] = {
                "emit": self.to_wall(tx + RX_LATENCY_NS),
                "lat": np.array([p[1] for p in pts]),
                "lon": np.array([p[2] for p in pts]),
                "alt": np.array([p[3] for p in pts]),
                "ground": np.array([p[4] for p in pts]),
            }

    def to_wall(self, rx_ns):
        return self.t0 + ((rx_ns - self.first_rx) / self.speed).astype(np.int64)

    def match(self, hx, t_obs, lat, lon):
        """Index of the frame a shown position came from, or None."""
        tr = self.by_hex.get(hx)
        if tr is None:
            return None
        ok = (tr["emit"] <= t_obs + 50_000_000) & (tr["emit"] >= t_obs - int(MATCH_WINDOW_S * 1e9))
        if not ok.any():
            return None
        d = dist_m(lat, lon, tr["lat"], tr["lon"])
        d = np.where(ok, d, np.inf)
        best = float(d.min())
        if best > MATCH_M:
            return None
        # Ties: a stationary or slow aircraft sends many frames at (almost) the
        # same position. Take the most recent of them, not the first, or a
        # parked aircraft would look like it took a minute to appear.
        close = np.flatnonzero(d <= best + 2.0)
        return int(close[-1])

    def position_at(self, hx, t_wall):
        """True position (interpolated) at a wall-clock time, or None outside the track."""
        tr = self.by_hex.get(hx)
        if tr is None:
            return None
        e = tr["emit"]
        if t_wall < e[0] or t_wall > e[-1]:
            return None
        j = int(np.searchsorted(e, t_wall))
        if j == 0:
            return tr["lat"][0], tr["lon"][0]
        f = (t_wall - e[j - 1]) / max(1, e[j] - e[j - 1])
        return (tr["lat"][j - 1] + f * (tr["lat"][j] - tr["lat"][j - 1]),
                tr["lon"][j - 1] + f * (tr["lon"][j] - tr["lon"][j - 1]))


def dist_m(lat1, lon1, lat2, lon2):
    dy = (np.asarray(lat2) - lat1) * EARTH_M_PER_DEG
    dx = (np.asarray(lon2) - lon1) * EARTH_M_PER_DEG * np.cos(np.radians(lat1))
    return np.hypot(dx, dy)


def load_obs(path):
    out = []
    if not path.exists():
        return out
    with open(path) as f:
        for r in csv.DictReader(f):
            out.append((int(r["t_ns"]), r["hex"].upper(), float(r["lat"]), float(r["lon"])))
    return out


def load_sbs(path):
    """readsb SBS lines -> positions [(t, hex, lat, lon, alt)] and identity {hex: {...}}."""
    pos, ident = [], defaultdict(dict)
    with open(path) as f:
        next(f, None)
        for line in f:
            t_s, _, msg = line.rstrip("\n").partition(",")
            p = msg.split(",")
            if len(p) < 18 or p[0] != "MSG":
                continue
            t, hx = int(t_s), p[4].strip().upper()
            if p[1] == "3" and p[14] and p[15]:
                pos.append((t, hx, float(p[14]), float(p[15]), int(p[11]) if p[11] else None))
            if p[10].strip():
                ident[hx]["callsign"] = p[10].strip()
            if p[17].strip():
                ident[hx]["squawk"] = p[17].strip()
    return pos, ident


def load_new_updates(path):
    """TrackUpdate stream from adsb-replay --out -> positions and identity."""
    data = path.read_bytes()
    off, pos, ident = 0, [], defaultdict(dict)
    while off + 2 <= len(data):
        (ln,) = struct.unpack_from("<H", data, off)
        msg = data[off + 2:off + 2 + ln]
        off += 2 + ln
        if ln != 64 or msg[0] != ord("U"):
            continue
        (mask, icao, ev_ts, _radio, lat_e7, lon_e7, alt, _gs, _trk, _vr, squawk) = struct.unpack_from(
            "<HIQQiiiHHhH", msg, 2)
        callsign = msg[44:52].decode("latin-1")
        hx = f"{icao:06X}"
        if mask & 1 and lat_e7 != -2**31:
            pos.append((ev_ts, hx, lat_e7 / 1e7, lon_e7 / 1e7, None if alt == -2**31 else alt))
        if mask & (1 << 5):
            ident[hx]["callsign"] = callsign.strip()
        if squawk != 0xFFFF:
            ident[hx]["squawk"] = f"{squawk:04X}"
    return pos, ident


def pct(a, qs=(50, 90, 99, 99.9)):
    a = np.asarray(a, dtype=float)
    if a.size == 0:
        return {f"p{q:g}": None for q in qs} | {"max": None, "n": 0}
    r = {f"p{q:g}": float(np.percentile(a, q)) for q in qs}
    r["max"] = float(a.max())
    r["mean"] = float(a.mean())
    r["n"] = int(a.size)
    return r


# ---------------------------------------------------------------------------
# One scenario
# ---------------------------------------------------------------------------

def latencies(truth, obs, t_min, per_second=None):
    lat_ms, unmatched, matched_idx = [], 0, defaultdict(set)
    for t, hx, la, lo in obs:
        if t < t_min:
            continue
        i = truth.match(hx, t, la, lo)
        if i is None:
            unmatched += 1
            continue
        ms = (t - truth.by_hex[hx]["emit"][i]) / 1e6
        lat_ms.append(ms)
        matched_idx[hx].add(i)
        if per_second is not None:
            per_second[int((t - truth.t0) / 1e9)].append(ms)
    return lat_ms, unmatched, matched_idx


def scenario(d):
    d = Path(d)
    feed = json.loads((d / "feed.json").read_text())
    truth = Truth(d / "truth.csv", feed)
    t_min = feed["t0_wall_ns"] + int(WARMUP_S * 1e9)
    t_end = feed["end_wall_ns"]
    m = {"aircraft": len(truth.by_hex), "frames_fed": feed["sent"],
         "feeder_max_lateness_ms": feed["max_lateness_ms"]}

    # Latency at each stage of the old pipeline.
    sbs_pos, sbs_ident = load_sbs(d / "sbs.csv")
    stages = {}
    for name, obs in [("readsb_sbs", [(t, h, la, lo) for t, h, la, lo, _ in sbs_pos]),
                      ("database", load_obs(d / "db.csv")),
                      ("api", load_obs(d / "api.csv"))]:
        per_second = defaultdict(list) if name == "database" else None
        lat, unmatched, idx = latencies(truth, obs, t_min, per_second)
        if per_second:
            # Worst one-second window: shows stalls that a percentile hides.
            sec, vals = max(per_second.items(), key=lambda kv: np.median(kv[1]))
            m["db_worst_second"] = {"t_offset_s": sec, "median_ms": float(np.median(vals)), "observations": len(vals)}
        stages[name] = {"latency_ms": pct(lat), "unmatched": unmatched,
                        "positions_shown": sum(len(v) for v in idx.values())}
        if name == "database":
            db_lat = lat
    # Browser: the map polls the API every 2 s, so add a uniform 0-2 s wait.
    rng = np.random.default_rng(0)
    api_lat = np.array([x for x in latencies(truth, load_obs(d / "api.csv"), t_min)[0]])
    browser = (api_lat[:, None] + rng.uniform(0, POLL_S * 1000, size=(api_lat.size, 16))).ravel() \
        if api_lat.size else np.array([])
    stages["browser_estimate"] = {"latency_ms": pct(browser)}
    m["stages"] = stages

    # Coverage: share of transmitted airborne positions that ever reach the DB.
    sent_airborne = 0
    for hx, tr in truth.by_hex.items():
        mask = (tr["emit"] >= t_min) & (tr["emit"] <= t_end) & (~tr["ground"])
        sent_airborne += int(mask.sum())
    m["positions_transmitted"] = sent_airborne
    for name in ("readsb_sbs", "database", "api"):
        shown = stages[name]["positions_shown"]
        stages[name]["coverage"] = shown / sent_airborne if sent_airborne else None

    # What the map shows, sampled once a second: age of the shown position and
    # its distance from where the aircraft actually is.
    ages, errs, errs_browser = [], [], []
    with open(d / "api_snapshots.csv") as f:
        for r in csv.DictReader(f):
            t, hx = int(r["t_ns"]), r["hex"].upper()
            if t < t_min or t > t_end:
                continue
            la, lo = float(r["lat"]), float(r["lon"])
            i = truth.match(hx, t, la, lo)
            now = truth.position_at(hx, t)
            if i is None or now is None:
                continue
            ages.append((t - truth.by_hex[hx]["emit"][i]) / 1e6)
            errs.append(float(dist_m(la, lo, now[0], now[1])))
            # In the browser the snapshot is held until the next poll.
            later = truth.position_at(hx, t + int(rng.uniform(0, POLL_S) * 1e9))
            if later is not None:
                errs_browser.append(float(dist_m(la, lo, later[0], later[1])))
    m["map_view"] = {"position_age_ms": pct(ages), "position_error_m": pct(errs),
                     "browser_position_error_m": pct(errs_browser)}

    # Is the old pipeline keeping up? Compare DB latency early vs late in the run.
    if db_lat:
        q = max(1, len(db_lat) // 4)
        m["falling_behind"] = {"db_latency_first_quarter_ms": float(np.median(db_lat[:q])),
                               "db_latency_last_quarter_ms": float(np.median(db_lat[-q:]))}

    # CPU use per process (cores), from /proc samples.
    cpu = defaultdict(list)
    with open(d / "cpu.csv") as f:
        for r in csv.DictReader(f):
            cpu[(r["name"], r["pid"])].append((int(r["t_ns"]), float(r["cpu_seconds"])))
    usage = defaultdict(float)
    for (name, _pid), samples in cpu.items():
        if len(samples) >= 2:
            (ta, ca), (tb, cb) = samples[0], samples[-1]
            usage[name] += (cb - ca) / max(1e-9, (tb - ta) / 1e9)
    m["cpu_cores"] = {k: round(v, 3) for k, v in usage.items()}

    # New pipeline on the same recording.
    new = {}
    for line in (d / "new_pipeline.txt").read_text().splitlines():
        parts = line.replace("release lag", "release_lag").split()
        if parts and parts[0] in ("release_lag", "queue", "decode+state", "total"):
            vals = dict(zip(parts[1::2], parts[2::2]))
            new[parts[0]] = {k: int(v) for k, v in vals.items() if v.isdigit()}
    m["new_pipeline_latency_ns"] = new

    # Decoder comparison: readsb vs the new decoder vs ground truth.
    new_pos, new_ident = load_new_updates(d / "new_updates.bin")
    rx_to_wall = lambda rx: truth.t0 + int((rx - truth.first_rx) / truth.speed)  # noqa: E731
    def accuracy(positions, timestamp_is_rx):
        within, total, alt_ok, alt_n = 0, 0, 0, 0
        for t, hx, la, lo, alt in positions:
            tw = rx_to_wall(t) if timestamp_is_rx else t
            i = truth.match(hx, tw + 50_000_000, la, lo)
            total += 1
            if i is not None:
                within += 1
                if alt is not None and not truth.by_hex[hx]["ground"][i]:
                    alt_n += 1
                    alt_ok += abs(alt - truth.by_hex[hx]["alt"][i]) <= 25
        return {"positions": total, "matched_within_m": MATCH_M,
                "match_rate": within / total if total else None,
                "altitude_within_25ft": alt_ok / alt_n if alt_n else None}
    agree_cs = agree_sq = both_cs = both_sq = 0
    for hx in set(sbs_ident) & set(new_ident):
        a, b = sbs_ident[hx], new_ident[hx]
        if "callsign" in a and "callsign" in b:
            both_cs += 1
            agree_cs += a["callsign"] == b["callsign"]
        if "squawk" in a and "squawk" in b:
            both_sq += 1
            agree_sq += a["squawk"] == b["squawk"]
    m["decoder_comparison"] = {
        "readsb": accuracy(sbs_pos, False) | {"aircraft": len({p[1] for p in sbs_pos})},
        "new": accuracy(new_pos, True) | {"aircraft": len({p[1] for p in new_pos})},
        "callsign_agreement": agree_cs / both_cs if both_cs else None,
        "squawk_agreement": agree_sq / both_sq if both_sq else None,
        "aircraft_with_both_callsigns": both_cs,
    }

    (d / "metrics.json").write_text(json.dumps(m, indent=2))
    s = m["stages"]
    f = lambda x: "-" if x is None else f"{x:,.0f}"  # noqa: E731
    print(f"{m['aircraft']} aircraft: readsb p50 {f(s['readsb_sbs']['latency_ms']['p50'])} ms | "
          f"DB p50 {f(s['database']['latency_ms']['p50'])} / p99 {f(s['database']['latency_ms']['p99'])} ms | "
          f"API p50 {f(s['api']['latency_ms']['p50'])} ms | browser p50 {f(s['browser_estimate']['latency_ms']['p50'])} ms | "
          f"DB coverage {100 * (s['database']['coverage'] or 0):.0f}% | "
          f"map error p50 {f(m['map_view']['position_error_m']['p50'])} m | cpu {m['cpu_cores']}")


# ---------------------------------------------------------------------------
# Report over all scenarios
# ---------------------------------------------------------------------------

def fmt(x, unit="", digits=0):
    if x is None:
        return "–"
    return f"{x:,.{digits}f}{unit}"


def report(out):
    out = Path(out)
    env = json.loads((out / "environment.json").read_text())
    runs = []
    for p in sorted(out.glob("n*/metrics.json"), key=lambda p: int(p.parent.name[1:])):
        runs.append(json.loads(p.read_text()))
    buffering = (out / "readsb_output_buffering.txt").read_text().strip() if (out / "readsb_output_buffering.txt").exists() else ""

    L = []
    w = L.append
    w("# Fall 2025 Baseline: Measured Results")
    w("")
    w("Generated by `bench/legacy/run.sh`. The unmodified Fall 2025 pipeline (Python streamer → Postgres → Node API → 2 s map poll) is fed synthetic traffic through readsb in real time. Every position it shows is matched to the transmitted frame it came from using the generator's ground truth. Method and caveats: [bench/legacy/README.md](../../legacy/README.md).")
    w("")
    w(f"Environment: {env['kernel']}, {env['cpus']} CPUs (Docker), {env['python']}, Node {env['node']}, {env['readsb_version']}. "
      f"{env['duration_s']} s of traffic per run, seed {env['seed']}, repo `{env['repo_commit']}`. "
      "The first 20 s of each run are excluded as warm-up.")
    w("")
    w("## Latency: frame received → visible")
    w("")
    w("| Aircraft | readsb SBS out (p50 / p99) | Database (p50 / p99) | API (p50 / p99) | **Map in browser** (p50 / p99, est.) |")
    w("|---|---|---|---|---|")
    for r in runs:
        s = r["stages"]
        g = lambda k: s[k]["latency_ms"]  # noqa: E731
        w(f"| {r['aircraft']:,} | {fmt(g('readsb_sbs')['p50'])} / {fmt(g('readsb_sbs')['p99'])} ms "
          f"| {fmt(g('database')['p50'])} / {fmt(g('database')['p99'])} ms "
          f"| {fmt(g('api')['p50'])} / {fmt(g('api')['p99'])} ms "
          f"| **{fmt(g('browser_estimate')['p50'])} / {fmt(g('browser_estimate')['p99'])} ms** |")
    w("")
    w("## What the map shows")
    w("")
    w("| Aircraft | Positions reaching the DB | Age of shown position (p50 / p99) | Shown vs. true position (p50 / p99) | In the browser, incl. poll wait (p50 / p99) |")
    w("|---|---|---|---|---|")
    for r in runs:
        mv = r["map_view"]
        cov = r["stages"]["database"]["coverage"]
        w(f"| {r['aircraft']:,} | {fmt(100 * cov if cov is not None else None, '%')} "
          f"| {fmt(mv['position_age_ms']['p50'])} / {fmt(mv['position_age_ms']['p99'])} ms "
          f"| {fmt(mv['position_error_m']['p50'])} / {fmt(mv['position_error_m']['p99'])} m "
          f"| {fmt(mv['browser_position_error_m']['p50'])} / {fmt(mv['browser_position_error_m']['p99'])} m |")
    w("")
    w("## Keeping up under load")
    w("")
    w("| Aircraft | Frames fed (per s) | Feeder lateness max | DB latency, first vs last quarter (median) | Worst 1 s window (DB median) | CPU: streamer | CPU: web server | CPU: readsb |")
    w("|---|---|---|---|---|---|---|---|")
    for r in runs:
        fb = r.get("falling_behind", {})
        c = r["cpu_cores"]
        rate = r["frames_fed"] / max(1, env["duration_s"])
        w(f"| {r['aircraft']:,} | {r['frames_fed']:,} ({rate:,.0f}/s) | {fmt(r['feeder_max_lateness_ms'], ' ms', 1)} "
          f"| {fmt(fb.get('db_latency_first_quarter_ms'))} → {fmt(fb.get('db_latency_last_quarter_ms'))} ms "
          f"| {fmt(r.get('db_worst_second', {}).get('median_ms'))} ms at {r.get('db_worst_second', {}).get('t_offset_s', '–')} s "
          f"| {fmt(100 * c.get('adsb_streamer_optimized.py', 0), '%')} | {fmt(100 * c.get('server.js', 0), '%')} | {fmt(100 * c.get('readsb', 0), '%')} |")
    w("")
    w("CPU is the share of one core.")
    w("")
    w("## New pipeline on the same recordings (for context)")
    w("")
    w("`adsb-replay --pipeline --speed 1`: the same frames at the same rate, from frame in to track state updated. The new pipeline has no publisher or map yet, so this is not yet an end-to-end comparison.")
    w("")
    w("| Aircraft | p50 | p99 | p99.9 | max | replay pacing lag p99 |")
    w("|---|---|---|---|---|---|")
    for r in runs:
        t = r["new_pipeline_latency_ns"].get("total", {})
        lag = r["new_pipeline_latency_ns"].get("release_lag", {})
        us = lambda k, src=t: None if k not in src else src[k] / 1000  # noqa: E731
        w(f"| {r['aircraft']:,} | {fmt(us('p50'), ' µs', 1)} | {fmt(us('p99'), ' µs', 1)} | {fmt(us('p99.9'), ' µs', 1)} "
          f"| {fmt(us('max'), ' µs', 1)} | {fmt(us('p99', lag), ' µs', 1)} |")
    w("")
    w("\"Replay pacing lag\" is how late the harness released frames. It is counted in the latency, so it must stay small for the numbers to mean anything.")
    w("")
    w("## Decoder comparison: readsb vs. new decoder vs. ground truth")
    w("")
    w(f"Positions are \"matched\" if within {MATCH_M:.0f} m of a transmitted position for that aircraft.")
    w("")
    w("| Aircraft | readsb positions (matched) | new positions (matched) | readsb altitude ±25 ft | new altitude ±25 ft | callsign agreement | squawk agreement |")
    w("|---|---|---|---|---|---|---|")
    for r in runs:
        dc = r["decoder_comparison"]
        a, b = dc["readsb"], dc["new"]
        p = lambda x: fmt(None if x is None else 100 * x, "%", 2)  # noqa: E731
        w(f"| {r['aircraft']:,} | {a['positions']:,} ({p(a['match_rate'])}) | {b['positions']:,} ({p(b['match_rate'])}) "
          f"| {p(a['altitude_within_25ft'])} | {p(b['altitude_within_25ft'])} | {p(dc['callsign_agreement'])} | {p(dc['squawk_agreement'])} |")
    w("")
    w("## Notes")
    w("")
    w("- **Lower bound on the real system:** Postgres runs locally, not on Supabase across the internet, so production latencies were higher.")
    w("- **Browser column:** estimated by adding the map's uniformly distributed 0–2 s poll wait to the measured API latency. Browser rendering time is not included.")
    w("- **Probe resolution:** the database and API probes poll every 100 ms, which adds about 50 ms on average.")
    w("- **Laptop numbers:** CPU and the throughput limit depend on the machine. Rerun on the benchmark host for the final report.")
    if buffering:
        w(f"- **readsb output buffering:** defaults, as on a stock receiver: `{' '.join(buffering.split())}`")
    w("")
    print("\n".join(L))


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("scenario", "report"):
        sys.exit("usage: analyze.py scenario DIR | analyze.py report OUT")
    (scenario if sys.argv[1] == "scenario" else report)(sys.argv[2])
