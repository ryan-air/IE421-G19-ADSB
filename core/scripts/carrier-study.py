#!/usr/bin/env python3
"""Transponder frequency-stability study from adsb-carrier's per-message CSV
(plan §22.5).

    carrier-study.py MESSAGES.csv [--rx-ppm X | --rx-estimate mean|none]
                     [--rx-drift BIN_S] [--types FILE] [--aircraft-out FILE]
                     [--truth SCENARIO.sigmf-meta] [--json] [--require RULE ...]

For every message, adsb-carrier measured the baseband carrier frequency f
(on the recording's own sample clock) and, when the aircraft had a position
and velocity, the Doppler D. With a receiver whose LO and sample clock share
one crystal that is `ppm` off,

    f (1 + ppm) = (fc + offset) (1 + D/fc') - fc (1 + ppm)
    offset ~= f (1 + ppm) - D + fc ppm              (fc' ~ fc; error < 1 Hz)

so the transmitter's offset from the nominal carrier needs the receiver's
ppm. It is either
  * known (--rx-ppm, a GPS-disciplined or calibrated receiver), or
  * estimated (--rx-estimate mean, the default): the value that makes the
    mean over aircraft of their mean offsets zero. Transponder offsets
    scatter by ~100 kHz, so with N aircraft this is only good to about
    sd / sqrt(N) / fc (printed): fine for drift and for differences between
    aircraft, not for absolute offsets.
  * --rx-estimate none: offsets relative to the receiver's LO.
Drift of the receiver itself (temperature) is common to every aircraft at
the same time: --rx-drift BIN_S fits it, one value per time bin (linear in
between), together with each aircraft's offset and drift, and removes it.
It is measured from t = 0 (the ppm is the LO error at the start). A common
linear trend cannot be told from the aircraft's own drifts, so their mean
drift is taken to be zero. Without receiver drift the option only adds
noise (about 0.1-0.2 Hz/s on the drifts in a 60 s recording).

Per aircraft: messages, span, offset at the first message and at t = 0,
linear drift (Hz/s) with its standard error, residual spread, and the
Allan deviation at 1, 2, 5, 10, 20, 50 s of the offset (adev_*: includes the
drift, b tau / sqrt 2) and of the residuals after the drift fit
(adev_detrended_*). Grouped by emitter
category (from the aircraft's identification messages) and, with
--types, by aircraft type: a CSV icao,type[,...] (e.g. from a public aircraft
database), or ads/aircraft-types-by-hex.json (US registrations -> FAA
aircraft class: fixed-single, fixed-multi, rotorcraft, ...).

Messages are used when coherence >= --min-coherence (0.9), sigma <=
--max-sigma (300 Hz) and the Doppler is known; per aircraft, residuals
beyond 5 MADs of the drift fit are dropped (collisions, multipath).

--truth: adsb-iqsynth's metadata. Each message is matched to its annotation
(same ICAO, sample position within 2 samples) and the errors are reported:
per-message frequency vs. the observed frequency, Doppler vs. truth, offset
vs. the transmitted offset; per aircraft offset at t = 0 and drift vs. the
transmitter truth. --require METRIC<=X or METRIC>=X checks any metric
printed in the summary (exit 1 if one fails).
"""

import argparse
import bisect
import collections
import csv
import json
import math
import sys

FC = 1090e6
# FAA aircraft registry TYPE-ACFT codes (ads/aircraft-types-by-hex.json).
FAA_TYPE_ACFT = {"1": "glider", "2": "balloon", "3": "blimp", "4": "fixed-single", "5": "fixed-multi",
                 "6": "rotorcraft", "7": "weight-shift", "8": "powered-parachute", "9": "gyroplane",
                 "H": "hybrid-lift", "O": "other"}
TAUS = [1, 2, 5, 10, 20, 50]


def percentile(xs, p):
    if not xs:
        return float("nan")
    s = sorted(xs)
    k = (len(s) - 1) * p / 100.0
    lo = math.floor(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def rms(xs):
    return math.sqrt(sum(x * x for x in xs) / len(xs)) if xs else float("nan")


def median(xs):
    return percentile(xs, 50)


def linfit(ts, ys, ws):
    """Weighted least squares y = a + b t; returns a, b, se_a, se_b, residual sd."""
    sw = sum(ws)
    tm = sum(w * t for w, t in zip(ws, ts)) / sw
    ym = sum(w * y for w, y in zip(ws, ys)) / sw
    stt = sum(w * (t - tm) ** 2 for w, t in zip(ws, ts))
    if stt <= 0 or len(ts) < 3:
        return ym, 0.0, float("nan"), float("nan"), float("nan")
    b = sum(w * (t - tm) * (y - ym) for w, t, y in zip(ws, ts, ys)) / stt
    a = ym - b * tm
    res = [y - a - b * t for t, y in zip(ts, ys)]
    n = len(ts)
    # Residual variance per unit weight (weights normalized to mean 1).
    wn = [w * n / sw for w in ws]
    s2 = sum(w * r * r for w, r in zip(wn, res)) / (n - 2)
    se_b = math.sqrt(s2 / (stt * n / sw))
    se_a = math.sqrt(s2 * (1.0 / n + tm * tm / (stt * n / sw)))
    return a, b, se_a, se_b, math.sqrt(sum(r * r for r in res) / (n - 2))


def allan(ts, ys, tau):
    """Allan deviation of an irregularly sampled frequency series: means over
    consecutive bins of length tau, differences of neighbouring non-empty bins."""
    if not ts:
        return float("nan"), 0
    t0 = ts[0]
    bins = collections.defaultdict(list)
    for t, y in zip(ts, ys):
        bins[int((t - t0) // tau)].append(y)
    means = {k: sum(v) / len(v) for k, v in bins.items()}
    diffs = [(means[k + 1] - means[k]) ** 2 for k in means if k + 1 in means]
    if len(diffs) < 3:
        return float("nan"), len(diffs)
    return math.sqrt(0.5 * sum(diffs) / len(diffs)), len(diffs)


def load_messages(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            row = {
                "t": float(r["time_s"]),
                "pos": float(r["sample_pos"]),
                "icao": r["icao"].upper(),
                "df": int(r["df"]),
                "snr": float(r["snr_db"]),
                "f": float(r["freq_hz"]),
                "sigma": float(r["sigma_hz"]),
                "coh": float(r["coherence"]),
                "doppler": float(r["doppler_hz"]) if r["doppler_hz"] else None,
                "cat": r["category"],
            }
            rows.append(row)
    return rows


def load_truth(path):
    with open(path) as f:
        meta = json.load(f)
    g = meta["global"]
    tx = {t["icao"].upper(): t for t in g.get("adsb:transmitters", [])}
    by_icao = collections.defaultdict(list)
    for a in meta.get("annotations", []):
        by_icao[a["adsb:icao"].upper()].append(a)
    for v in by_icao.values():
        v.sort(key=lambda a: a["adsb:sample_pos"])
    return {
        "ppm": g.get("adsb:receiver", {}).get("ppm"),
        "lo_drift": g.get("adsb:receiver", {}).get("lo_drift_ppm_per_s"),
        "jitter": g.get("adsb:generator", {}).get("tx_jitter_sd_hz"),
        "tx": tx,
        "ann": by_icao,
        "fc": meta.get("captures", [{}])[0].get("core:frequency", FC),
    }


def match(truth, icao, pos):
    anns = truth["ann"].get(icao)
    if not anns:
        return None
    keys = [a["adsb:sample_pos"] for a in anns]
    i = bisect.bisect_left(keys, pos)
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(anns) and abs(keys[j] - pos) < 2.0:
            if best is None or abs(keys[j] - pos) < abs(best["adsb:sample_pos"] - pos):
                best = anns[j]
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("messages")
    ap.add_argument("--rx-ppm", type=float, help="known receiver LO/sample-clock error")
    ap.add_argument("--rx-estimate", choices=["mean", "none"], default="mean")
    ap.add_argument("--rx-drift", type=float, default=0.0, help="estimate receiver drift in bins of this many s")
    ap.add_argument("--center", type=float, help="tuned frequency (default: the truth's core:frequency, else 1090e6)")
    ap.add_argument("--min-coherence", type=float, default=0.9)
    ap.add_argument("--max-sigma", type=float, default=300.0)
    ap.add_argument("--min-messages", type=int, default=20, help="per aircraft, for the per-aircraft table")
    ap.add_argument("--types", help="CSV icao,type[,...] or ads/aircraft-types-by-hex.json, for grouping by type")
    ap.add_argument("--aircraft-out", help="write the per-aircraft table as CSV")
    ap.add_argument("--truth", help="adsb-iqsynth SigMF metadata")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--require", action="append", default=[], help="METRIC<=X or METRIC>=X")
    args = ap.parse_args()
    fc = args.center
    if fc is None and args.truth:
        with open(args.truth) as f:
            fc = json.load(f).get("captures", [{}])[0].get("core:frequency")
    fc = fc or FC

    rows = load_messages(args.messages)
    metrics = collections.OrderedDict()
    metrics["messages"] = len(rows)
    used = [r for r in rows if r["coh"] >= args.min_coherence and r["sigma"] <= args.max_sigma
            and r["doppler"] is not None]
    metrics["messages_used"] = len(used)
    metrics["low_coherence"] = sum(1 for r in rows if r["coh"] < args.min_coherence)
    # Aircraft whose messages are mostly incoherent: a transmitter whose phase
    # jumps between pulses (e.g. an old cavity-oscillator transponder) has no
    # carrier to measure across a message; listed, not measured.
    all_by_ac = collections.defaultdict(list)
    for r in rows:
        all_by_ac[r["icao"]].append(r["coh"])
    incoherent = sorted(icao for icao, c in all_by_ac.items()
                        if len(c) >= args.min_messages and sum(1 for x in c if x < args.min_coherence) > len(c) / 2)
    metrics["incoherent_aircraft"] = len(incoherent)

    # Offsets relative to the receiver LO first (ppm = 0), then shift.
    def offset(r, ppm):
        return r["f"] * (1 + ppm * 1e-6) - r["doppler"] + fc * ppm * 1e-6

    by_ac = collections.defaultdict(list)
    for r in used:
        by_ac[r["icao"]].append(r)
    for v in by_ac.values():
        v.sort(key=lambda r: r["t"])

    if args.rx_ppm is not None:
        ppm, ppm_se, ppm_how = args.rx_ppm, 0.0, "given"
    elif args.rx_estimate == "mean":
        means = [sum(offset(r, 0.0) for r in v) / len(v) for v in by_ac.values() if len(v) >= 3]
        if len(means) >= 2:
            m = sum(means) / len(means)
            sd = math.sqrt(sum((x - m) ** 2 for x in means) / (len(means) - 1))
            # offset(ppm) ~= offset(0) + fc ppm: choose ppm so the mean is 0.
            ppm = -m / fc * 1e6
            ppm_se = sd / math.sqrt(len(means)) / fc * 1e6
        else:
            ppm, ppm_se = 0.0, float("nan")
        ppm_how = f"estimated from {len(means)} aircraft means"
    else:
        ppm, ppm_se, ppm_how = 0.0, float("nan"), "not removed (offsets relative to the receiver LO)"
    metrics["rx_ppm"] = ppm
    metrics["rx_ppm_se"] = ppm_se
    for r in used:
        r["off"] = offset(r, ppm)

    # Receiver drift: median deviation of every aircraft from its own mean, per
    # time bin, interpolated linearly between bin centres.
    rx_drift = {}

    def rxd(t):
        """R(t): linear between bin centres; beyond the first or last centre,
        the line through the two nearest bins; next to a missing bin, the
        bin on the other side."""
        if not rx_drift:
            return 0.0
        x = t / args.rx_drift - 0.5
        b = math.floor(x)
        lo, hi = rx_drift.get(b), rx_drift.get(b + 1)
        if lo is not None and hi is not None:
            return lo + (hi - lo) * (x - b)
        keys = sorted(rx_drift)
        if len(keys) >= 2 and (x < keys[0] or x > keys[-1]):
            k0, k1 = (keys[0], keys[1]) if x < keys[0] else (keys[-2], keys[-1])
            return rx_drift[k0] + (rx_drift[k1] - rx_drift[k0]) * (x - k0) / (k1 - k0)
        if lo is not None or hi is not None:
            return lo if lo is not None else hi
        return rx_drift[min(keys, key=lambda k: abs(k - x))]

    if args.rx_drift > 0:
        # Alternating least squares of off = A_i + B_i t + R(t): each aircraft's
        # offset and drift given R, then R per bin (median over aircraft of
        # their mean residual in the bin) given the fits. A common linear
        # trend cannot be told apart from the aircraft's own drifts, so the
        # mean drift over aircraft is assigned to the receiver (transponder
        # drifts average to zero, as their offsets do for --rx-estimate mean).
        fleet = [v for v in by_ac.values() if len(v) >= 10 and v[-1]["t"] - v[0]["t"] >= 2 * args.rx_drift]
        for _ in range(20):
            fits = []
            for v in fleet:
                ts = [r["t"] for r in v]
                a, b_, _, _, _ = linfit(ts, [r["off"] - rxd(r["t"]) for r in v], [1.0] * len(v))
                fits.append((a, b_))
            if not fits:
                break
            mean_b = sum(f[1] for f in fits) / len(fits)
            dev = collections.defaultdict(list)
            for v, (a, b_) in zip(fleet, fits):
                per_bin = collections.defaultdict(list)
                for r in v:
                    per_bin[int(r["t"] // args.rx_drift)].append(r["off"] - a - (b_ - mean_b) * r["t"])
                for k, xs in per_bin.items():
                    dev[k].append(sum(xs) / len(xs))
            new_rx = {k: median(xs) for k, xs in dev.items() if len(xs) >= 3}
            if new_rx:
                m = sum(new_rx.values()) / len(new_rx)
                new_rx = {k: x - m for k, x in new_rx.items()}  # the constant part is the ppm's
            change = max((abs(new_rx[k] - rx_drift.get(k, 0.0)) for k in new_rx), default=0.0)
            rx_drift = new_rx
            if change < 0.01:
                break
        # Anchor R(0) = 0: a given (or estimated) ppm is the LO error at the
        # start of the recording, so the drift is measured from there.
        if rx_drift:
            r0 = rxd(0.0)
            rx_drift = {k: x - r0 for k, x in rx_drift.items()}
        if args.rx_ppm is None and args.rx_estimate == "mean":
            # Re-estimate the ppm (at t = 0) with the drift removed.
            means = [sum(offset(r, ppm) - rxd(r["t"]) for r in v) / len(v) for v in by_ac.values() if len(v) >= 3]
            if means:
                ppm -= sum(means) / len(means) / fc * 1e6
                metrics["rx_ppm"] = ppm
        for r in used:
            r["off"] = offset(r, ppm) - rxd(r["t"])
        if rx_drift:
            metrics["rx_drift_range_hz"] = max(rx_drift.values()) - min(rx_drift.values())
            if len(rx_drift) >= 3:
                bs = sorted(rx_drift)
                _, slope, _, se, _ = linfit([(b + 0.5) * args.rx_drift for b in bs], [rx_drift[b] for b in bs],
                                            [1.0] * len(bs))
                # The LO going up by x ppm lowers every offset by fc x.
                metrics["rx_lo_drift_ppm_per_s"] = -slope / fc * 1e6
                metrics["rx_lo_drift_se_ppm_per_s"] = se / fc * 1e6

    types = {}
    if args.types and args.types.endswith(".json"):
        # ads/aircraft-types-by-hex.json: ICAO -> FAA registry TYPE-ACFT code.
        with open(args.types) as f:
            for k, v in json.load(f).items():
                types[k.upper()] = FAA_TYPE_ACFT.get(str(v), str(v))
    elif args.types:
        with open(args.types, newline="") as f:
            for rec in csv.reader(f):
                if len(rec) >= 2 and not rec[0].startswith("#"):
                    types[rec[0].strip().upper()] = rec[1].strip()

    aircraft = []
    for icao, v in by_ac.items():
        if len(v) < args.min_messages:
            continue
        ts = [r["t"] for r in v]
        ys = [r["off"] for r in v]
        ws = [1.0 / (r["sigma"] ** 2 + 1.0) for r in v]
        a, b, se_a, se_b, sd = linfit(ts, ys, ws)
        # Drop outliers (5 MADs) and refit.
        res = [y - a - b * t for t, y in zip(ts, ys)]
        mad = median([abs(x) for x in res]) * 1.4826
        if mad > 0:
            keep = [i for i, x in enumerate(res) if abs(x) <= 5 * mad]
            if len(keep) >= 3 and len(keep) < len(ts):
                ts = [ts[i] for i in keep]
                ys = [ys[i] for i in keep]
                ws = [ws[i] for i in keep]
                a, b, se_a, se_b, sd = linfit(ts, ys, ws)
        # Weights with the aircraft's own jitter (spread beyond the estimates' sigma).
        extra = max(0.0, sd * sd - median([1.0 / w for w in ws]))
        ws = [1.0 / (1.0 / w + extra) for w in ws]
        a, b, se_a, se_b, sd = linfit(ts, ys, ws)
        cats = collections.Counter(r["cat"] for r in v if r["cat"] not in ("", "00"))
        ac = {
            "icao": icao,
            "messages": len(ts),
            "span_s": ts[-1] - ts[0],
            "offset_t0_hz": a,
            "offset_t0_se_hz": se_a,
            "offset_first_hz": a + b * ts[0],
            "drift_hz_per_s": b,
            "drift_se_hz_per_s": se_b,
            "residual_sd_hz": sd,
            "category": cats.most_common(1)[0][0] if cats else "",
            "type": types.get(icao, ""),
            "median_snr_db": median([r["snr"] for r in v]),
        }
        # Allan deviation of the offsets (the linear drift adds b tau / sqrt 2)
        # and of the residuals after the drift fit (the transmitter's noise).
        res = [y - a - b * t for t, y in zip(ts, ys)]
        for tau in TAUS:
            ac[f"adev_{tau}s_hz"], _ = allan(ts, ys, tau)
            ac[f"adev_detrended_{tau}s_hz"], _ = allan(ts, res, tau)
        aircraft.append(ac)
    aircraft.sort(key=lambda a: a["icao"])
    metrics["aircraft"] = len(aircraft)
    if aircraft:
        offs = [a["offset_t0_hz"] for a in aircraft]
        mo = sum(offs) / len(offs)
        metrics["offset_sd_hz"] = math.sqrt(sum((x - mo) ** 2 for x in offs) / max(1, len(offs) - 1))
        metrics["offset_abs_p50_hz"] = percentile([abs(x) for x in offs], 50)
        metrics["offset_abs_p95_hz"] = percentile([abs(x) for x in offs], 95)
        metrics["offset_abs_max_hz"] = max(abs(x) for x in offs)
        metrics["outside_1mhz"] = sum(1 for x in offs if abs(x) > 1e6)
        drifts = [a["drift_hz_per_s"] for a in aircraft if a["span_s"] >= 20]
        if drifts:
            metrics["drift_abs_p50_hz_per_s"] = percentile([abs(x) for x in drifts], 50)
        metrics["residual_sd_p50_hz"] = percentile([a["residual_sd_hz"] for a in aircraft], 50)

    # Truth.
    if args.truth:
        truth = load_truth(args.truth)
        if truth["ppm"] is not None:
            metrics["rx_ppm_true"] = truth["ppm"]
            if args.rx_estimate != "none" or args.rx_ppm is not None:
                metrics["rx_ppm_err"] = abs(ppm - truth["ppm"])
                if ppm_se > 0:
                    metrics["rx_ppm_z"] = metrics["rx_ppm_err"] / ppm_se
        f_err, z, d_err, o_err = [], [], [], []
        unmatched = 0
        for r in rows:
            t = match(truth, r["icao"], r["pos"])
            if t is None:
                unmatched += 1
                continue
            if t.get("adsb:collided"):
                continue
            f_err.append(r["f"] - t["adsb:observed_hz"])
            z.append((r["f"] - t["adsb:observed_hz"]) / max(r["sigma"], 1e-9))
            if r["doppler"] is not None:
                d_err.append(r["doppler"] - t["adsb:doppler_hz"])
            if "off" in r:
                o_err.append(r["off"] - t["adsb:tx_offset_hz"])
        good = [x for x in f_err]
        metrics["truth_unmatched"] = unmatched
        metrics["msg_err_rms_hz"] = rms(good)
        metrics["msg_err_p50_hz"] = percentile([abs(x) for x in good], 50)
        metrics["msg_err_p95_hz"] = percentile([abs(x) for x in good], 95)
        metrics["msg_z_p50"] = percentile([abs(x) for x in z], 50)  # ~0.674 if sigma is right
        metrics["doppler_err_p50_hz"] = percentile([abs(x) for x in d_err], 50)
        metrics["doppler_err_p95_hz"] = percentile([abs(x) for x in d_err], 95)
        metrics["tx_err_p50_hz"] = percentile([abs(x) for x in o_err], 50)
        a_err, a_z, dr_err, dr_z = [], [], [], []
        for a in aircraft:
            tx = truth["tx"].get(a["icao"])
            if not tx:
                continue
            a_err.append(a["offset_t0_hz"] - tx["offset_hz"])
            a_z.append(a_err[-1] / a["offset_t0_se_hz"])
            if a["span_s"] >= 20 and a["messages"] >= 50:
                dr_err.append(a["drift_hz_per_s"] - tx["drift_hz_per_s"])
                dr_z.append(dr_err[-1] / a["drift_se_hz_per_s"])
            a["true_offset_hz"] = tx["offset_hz"]
            a["true_drift_hz_per_s"] = tx["drift_hz_per_s"]
        if a_err:
            # The common receiver term (ppm error) is removed to see aircraft-to-aircraft precision.
            mean = sum(a_err) / len(a_err)
            metrics["offset_err_rms_hz"] = rms(a_err)
            metrics["offset_err_mean_hz"] = mean
            metrics["offset_err_mean_abs_hz"] = abs(mean)
            metrics["offset_err_rel_rms_hz"] = rms([x - mean for x in a_err])
            # Robust: a weak aircraft seen briefly has a large (and honestly reported) error.
            metrics["offset_err_rel_p50_hz"] = percentile([abs(x - mean) for x in a_err], 50)
            metrics["offset_err_rel_p90_hz"] = percentile([abs(x - mean) for x in a_err], 90)
            metrics["offset_z_p50"] = percentile([abs(x) for x in a_z], 50)  # ~0.674 if the se is right
            # The same without the common (receiver) term, which no aircraft's se includes.
            ses = [x / z for x, z in zip(a_err, a_z) if z != 0]
            metrics["offset_rel_z_p50"] = percentile([abs(x - mean) / se for x, se in zip(a_err, ses)], 50)
        if dr_err:
            metrics["drift_err_rms_hz_per_s"] = rms(dr_err)
            metrics["drift_err_p50_hz_per_s"] = percentile([abs(x) for x in dr_err], 50)
            metrics["drift_z_p50"] = percentile([abs(x) for x in dr_z], 50)
        if truth["lo_drift"] is not None:
            metrics["rx_lo_drift_true_ppm_per_s"] = truth["lo_drift"]
            if "rx_lo_drift_ppm_per_s" in metrics:
                metrics["rx_lo_drift_err_ppm_per_s"] = abs(metrics["rx_lo_drift_ppm_per_s"] - truth["lo_drift"])
        if truth["jitter"] is not None:
            metrics["jitter_true_hz"] = truth["jitter"]

    if args.aircraft_out and aircraft:
        with open(args.aircraft_out, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(aircraft[0].keys()) + [k for k in ("true_offset_hz", "true_drift_hz_per_s") if k not in aircraft[0]])
            w.writeheader()
            for a in aircraft:
                w.writerow({k: (f"{v:.6g}" if isinstance(v, float) else v) for k, v in a.items()})

    if args.json:
        print(json.dumps({"metrics": metrics, "aircraft": aircraft}, indent=1, default=str))
    else:
        print(f"messages: {len(rows)}, used {len(used)} (coherence >= {args.min_coherence}, sigma <= "
              f"{args.max_sigma:g} Hz, with Doppler); {metrics['low_coherence']} below the coherence limit")
        se = f" +- {ppm_se:.2f}" if ppm_se == ppm_se and ppm_se > 0 else ""
        print(f"receiver: {ppm:+.3f}{se} ppm ({ppm_how})")
        if rx_drift:
            line = f"receiver drift (median of aircraft, {args.rx_drift:g} s bins): range {metrics['rx_drift_range_hz']:.1f} Hz"
            if "rx_lo_drift_ppm_per_s" in metrics:
                line += (f", linear {metrics['rx_lo_drift_ppm_per_s'] * 1e3:+.3f} +- "
                         f"{metrics['rx_lo_drift_se_ppm_per_s'] * 1e3:.3f} ppb/s")
            print(line)
        print(f"aircraft with >= {args.min_messages} messages: {len(aircraft)}")
        if incoherent:
            print(f"  mostly incoherent (phase jumps between pulses; not measured): {len(incoherent)}: "
                  + " ".join(incoherent))
        if aircraft:
            print(f"  offset at t=0: sd {metrics['offset_sd_hz']/1e3:.1f} kHz, |offset| p50 {metrics['offset_abs_p50_hz']/1e3:.1f} kHz, "
                  f"p95 {metrics['offset_abs_p95_hz']/1e3:.1f} kHz, max {metrics['offset_abs_max_hz']/1e3:.1f} kHz, "
                  f"outside +-1 MHz: {metrics['outside_1mhz']}")
            if "drift_abs_p50_hz_per_s" in metrics:
                print(f"  |drift| p50 {metrics['drift_abs_p50_hz_per_s']:.3f} Hz/s (aircraft seen >= 20 s)")
            print(f"  residual spread p50 {metrics['residual_sd_p50_hz']:.1f} Hz")
            print("\n  icao    msgs  span_s  offset_kHz      drift_Hz/s       resid_Hz  adev10s*_Hz cat  type   (* drift removed)")
            for a in aircraft:
                print(f"  {a['icao']}  {a['messages']:4d}  {a['span_s']:6.1f}  {a['offset_t0_hz']/1e3:+9.3f}  "
                      f"{a['drift_hz_per_s']:+7.3f} +-{a['drift_se_hz_per_s']:.3f}  {a['residual_sd_hz']:8.1f}  "
                      f"{a['adev_detrended_10s_hz']:10.1f}  {a['category'] or '--':>3}  {a['type']}")
            for key, label in (("category", "emitter category"), ("type", "type")):
                groups = collections.defaultdict(list)
                for a in aircraft:
                    if a[key]:
                        groups[a[key]].append(a)
                if not groups:
                    continue
                print(f"\n  by {label} (n, offset sd kHz, |offset| p50 kHz, |drift| p50 Hz/s, residual p50 Hz):")
                for k in sorted(groups):
                    g = groups[k]
                    offs = [a["offset_t0_hz"] for a in g]
                    m = sum(offs) / len(offs)
                    sd = math.sqrt(sum((x - m) ** 2 for x in offs) / (len(offs) - 1)) if len(offs) > 1 else float("nan")
                    print(f"  {k:>17}  {len(g):3d}  {sd/1e3:8.1f}  {percentile([abs(x) for x in offs], 50)/1e3:8.1f}  "
                          f"{percentile([abs(a['drift_hz_per_s']) for a in g], 50):8.3f}  "
                          f"{percentile([a['residual_sd_hz'] for a in g], 50):8.1f}")
        if args.truth:
            print("\ntruth:")
            for k, v in metrics.items():
                if k.startswith(("truth_", "msg_", "doppler_", "tx_err", "offset_err", "offset_z", "offset_rel_z", "drift_err", "drift_z", "rx_ppm_", "rx_lo_", "jitter_")):
                    print(f"  {k} = {v:.4g}" if isinstance(v, float) else f"  {k} = {v}")

    failed = False
    for rule in args.require:
        for op in ("<=", ">="):
            if op in rule:
                name, val = rule.split(op, 1)
                break
        else:
            print(f"bad --require rule: {rule}", file=sys.stderr)
            return 2
        if name not in metrics:
            print(f"FAIL: {rule}: no metric {name}", file=sys.stderr)
            failed = True
            continue
        v = metrics[name]
        ok = (v <= float(val)) if op == "<=" else (v >= float(val))
        if not (v == v) or not ok:
            print(f"FAIL: {rule} (value {v:.6g})", file=sys.stderr)
            failed = True
    if args.require and not failed:
        print(f"OK: {len(args.require)} requirement(s) met")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
