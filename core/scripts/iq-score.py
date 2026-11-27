#!/usr/bin/env python3
"""Score a decoder's output against adsb-iqsynth's SigMF truth.

    iq-score.py SCENARIO.sigmf-meta DECODED [--json] [--require RULE ...]

DECODED is either
  * a Beast binary stream (readsb --net-connector ...,beast_out): messages
    carry 12 MHz timestamps counted from the first sample, so each one is
    matched to the truth annotation with the same hex at the same time
    (the decoder's constant timestamp offset is estimated from messages
    whose hex occurs once, then +-4 samples are allowed). The timing error
    is reported too.
  * raw text, one message per line as readsb/dump1090 print with --raw
    ("*8D4840D6202CC371C32CE0576098;"). Without times, matching is by hex
    only. DF11 replies (and other unchanged messages) repeat the same hex,
    so a missed early copy is charged to the last one: band rates are only
    exact for Beast input.
Decoded messages with no matching truth are "false" decodes.

Mode A/C replies (adsb-iqsynth --modeac-rate; truth labels MODEA / MODEC,
adsb:hex = the 2-byte code word) are scored separately from Mode S, so the
Mode S numbers do not change when a scenario or a decoder adds them. Beast
type '1' frames (and 4-digit raw lines) are Mode A/C. With Beast input each
decoded reply is matched to a truth reply at the same time (its own
timestamp offset, estimated from the data: readsb stamps F2, 20.3 us after
F1) with the same code; one at the time of an unmatched truth reply but with
another code is a wrong code, any other a false detection.

Prints the decode rate by SNR band, for clean and collided messages, and by
downlink format. --require checks thresholds and exits 1 if one fails:
    clean>=20:0.95   clean messages with SNR >= 20 dB decoded at >= 95%
    clean>=15<30:0.97  the same within an SNR band (15 <= SNR < 30 dB)
    false<=0         at most 0 false decodes
    false17<=0       at most 0 false DF17 decodes (DF17 has a plain CRC, so a
                     false one means wrong bits; DF11/16/20/21 parity can be
                     satisfied by noise, see readsb)
    all>=0:0.5       every message with SNR >= 0 dB decoded at >= 50%
    timing<=1.0      Beast input: every matched timestamp within 1 sample
    acclean>=15<30:0.95  clean Mode A/C replies (15 <= SNR < 30 dB) decoded
                     with the right code at >= 95%
    acwrong<=0.01    wrong codes at most 1% of the Mode A/C replies found
    acfalse<=N       at most N false Mode A/C detections
    acfalsepm<=X     at most X false Mode A/C detections per minute of recording
"""

import argparse
import collections
import json
import re
import sys

BANDS = [(-99, 0), (0, 5), (5, 10), (10, 15), (15, 20), (20, 30), (30, 99)]


def parse_beast(data):
    """(timestamp, HEX) for every Mode S frame in a Beast byte stream.

    A frame starts at a 0x1A that is not an escape: inside frames a data
    byte 0x1A is sent twice. The stream is first split into frames on those
    starts, whatever their type (status frames 0x34 and others included), so
    a doubled 0x1A inside any frame can never be taken for a frame start.
    Only types 0x31 (Mode A/C), 0x32 (Mode S short) and 0x33 (long) with a
    complete body are returned.
    """
    lengths = {0x31: 2, 0x32: 7, 0x33: 14}
    out, i, n = [], 0, len(data)
    frame = None  # [type, unescaped body] of the frame being collected

    def finish(f):
        if f and f[0] in lengths and len(f[1]) == 7 + lengths[f[0]]:
            out.append((int.from_bytes(f[1][:6], "big"), f[1][7:].hex().upper()))

    while i < n:
        c = data[i]
        if c == 0x1A:
            if i + 1 < n and data[i + 1] == 0x1A:
                if frame is not None:
                    frame[1].append(0x1A)  # escaped data byte
                i += 2
                continue
            finish(frame)
            frame = [data[i + 1], bytearray()] if i + 1 < n else None
            i += 2
            continue
        if frame is not None:
            frame[1].append(c)
        i += 1
    finish(frame)
    return out


def load(meta_path, decoded_path):
    with open(meta_path) as f:
        meta = json.load(f)
    truth = meta["annotations"]
    with open(decoded_path, "rb") as f:
        data = f.read()
    if b"\x1a" in data[:4096]:
        return meta, truth, parse_beast(data), True
    pat = re.compile(r"^\*([0-9A-Fa-f]{4}|[0-9A-Fa-f]{14}|[0-9A-Fa-f]{28});")
    decoded = []
    for line in data.decode(errors="replace").splitlines():
        m = pat.match(line.strip())
        if m:
            decoded.append((None, m.group(1).upper()))
    return meta, truth, decoded, False


def score_by_hex(truth, decoded):
    remaining = collections.Counter(h for _, h in decoded)
    for a in truth:
        h = a["adsb:hex"]
        a["_hit"] = remaining[h] > 0
        if a["_hit"]:
            remaining[h] -= 1
    return list(remaining.elements()), None


def score_by_time(truth, decoded, ticks_per_sample, tolerance=4.0):
    by_hex = collections.defaultdict(list)
    for a in truth:
        a["_hit"] = False
        by_hex[a["adsb:hex"]].append(a)
    # Constant offset of the decoder's timestamps (its reference point in the
    # message), from hexes that occur exactly once in the truth.
    diffs = sorted(ts / ticks_per_sample - by_hex[h][0]["adsb:sample_pos"]
                   for ts, h in decoded if len(by_hex.get(h, ())) == 1)
    if not diffs:
        return [h for _, h in decoded], None
    offset = diffs[len(diffs) // 2]
    false, errors, late_copies = [], [], 0
    for ts, h in decoded:
        pos = ts / ticks_per_sample - offset
        best = None
        for a in by_hex.get(h, ()):
            d = abs(pos - a["adsb:sample_pos"])
            if not a["_hit"] and d <= tolerance and (best is None or d < best[0]):
                best = (d, a)
        if best is None:
            false.append(h)
            # A copy of a real message decoded later than it arrived: an echo
            # (multipath) or a second detection of the same reply.
            if any(0 < pos - a["adsb:sample_pos"] < 64 for a in by_hex.get(h, ())):
                late_copies += 1
        else:
            best[1]["_hit"] = True
            errors.append(pos - best[1]["adsb:sample_pos"])
    errors.sort()
    timing = {"offset_samples": offset, "matched": len(errors), "late_copies": late_copies,
              "error_p50": errors[len(errors) // 2] if errors else None,
              "error_abs_max": max((abs(e) for e in errors), default=None)}
    return false, timing


def is_ac(a):
    return a["core:label"].startswith("MODE")


def score_ac_by_time(truth, decoded, ticks_per_sample, tolerance=4.0):
    """Mode A/C: (false, wrong, timing). Codes repeat (an aircraft answers
    many interrogations with the same code), so the timestamp offset is the
    densest 0.5-sample bin of all same-code differences within 40 us,
    refined by their median."""
    window = max(100.0, 40e-6 * 12e6 / ticks_per_sample)
    by_hex = collections.defaultdict(list)
    for a in truth:
        a["_hit"] = False
        by_hex[a["adsb:hex"]].append(a)
    for v in by_hex.values():
        v.sort(key=lambda a: a["adsb:sample_pos"])
    positions = sorted((a["adsb:sample_pos"], a) for a in truth)
    keys = [p for p, _ in positions]
    import bisect

    def near(pool_keys, pool, pos, span):
        lo = bisect.bisect_left(pool_keys, pos - span)
        hi = bisect.bisect_right(pool_keys, pos + span)
        return pool[lo:hi]

    hex_keys = {h: [a["adsb:sample_pos"] for a in v] for h, v in by_hex.items()}
    diffs = []
    for ts, h in decoded:
        pos = ts / ticks_per_sample
        for a in near(hex_keys.get(h, []), by_hex.get(h, []), pos, window):
            diffs.append(pos - a["adsb:sample_pos"])
    if not diffs:
        return [h for _, h in decoded], [], None
    bins = collections.Counter(round(d * 2) for d in diffs)
    centre = bins.most_common(1)[0][0] / 2
    close = sorted(d for d in diffs if abs(d - centre) <= tolerance)
    offset = close[len(close) // 2]
    false, wrong, errors = [], [], []
    for ts, h in decoded:
        pos = ts / ticks_per_sample - offset
        best = None
        for a in near(hex_keys.get(h, []), by_hex.get(h, []), pos, tolerance):
            d = abs(pos - a["adsb:sample_pos"])
            if not a["_hit"] and (best is None or d < best[0]):
                best = (d, a)
        if best is not None:
            best[1]["_hit"] = True
            errors.append(pos - best[1]["adsb:sample_pos"])
            continue
        other = [a for _, a in near(keys, positions, pos, tolerance) if not a["_hit"]]
        if other:
            a = min(other, key=lambda a: abs(pos - a["adsb:sample_pos"]))
            a["_wrong"] = True
            wrong.append((h, a["adsb:hex"]))
        else:
            false.append(h)
    errors.sort()
    timing = {"offset_samples": offset, "matched": len(errors),
              "error_p50": errors[len(errors) // 2] if errors else None,
              "error_abs_max": max((abs(e) for e in errors), default=None)}
    return false, wrong, timing


def score_ac_by_hex(truth, decoded):
    false, _ = score_by_hex(truth, decoded)
    return false, [], None


def rate(rows):
    n = len(rows)
    k = sum(1 for a in rows if a["_hit"])
    return k, n, (k / n if n else float("nan"))


def select(truth, kind, lo, hi=None):
    rows = [a for a in truth if a["adsb:snr_db"] >= lo and (hi is None or a["adsb:snr_db"] < hi)]
    if kind == "clean":
        rows = [a for a in rows if not a["adsb:collided"]]
    elif kind == "collided":
        rows = [a for a in rows if a["adsb:collided"]]
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("meta")
    ap.add_argument("decoded")
    ap.add_argument("--json", action="store_true", help="print the summary as JSON")
    ap.add_argument("--df", type=int, choices=range(32), action="append", metavar="N",
                    help="score only these Mode S downlink formats (repeatable)")
    ap.add_argument("--require", action="append", default=[], help="KIND>=SNR[<SNR]:RATE, false<=N, false17<=N or timing<=SAMPLES")
    args = ap.parse_args()

    meta, truth_all, decoded_all, timed = load(args.meta, args.decoded)
    truth = [a for a in truth_all if not is_ac(a)]
    ac_truth = [a for a in truth_all if is_ac(a)]
    decoded = [d for d in decoded_all if len(d[1]) != 4]
    ac_decoded = [d for d in decoded_all if len(d[1]) == 4]
    if args.df:
        truth = [a for a in truth if int(a["adsb:hex"][:2], 16) >> 3 in args.df]
        decoded = [d for d in decoded if int(d[1][:2], 16) >> 3 in args.df]
        ac_truth, ac_decoded = [], []
    if timed:
        ticks = 12e6 / meta["global"]["core:sample_rate"]
        false_hex, timing = score_by_time(truth, decoded, ticks)
    else:
        false_hex, timing = score_by_hex(truth, decoded)
    false = len(false_hex)
    false_by_df = collections.Counter(f"DF{int(h[:2], 16) >> 3}" for h in false_hex)
    seconds = meta["global"].get("adsb:stats", {}).get("samples", 0) / meta["global"]["core:sample_rate"]
    ac = None
    if ac_truth or ac_decoded:
        if timed:
            ac_false, ac_wrong, ac_timing = score_ac_by_time(ac_truth, ac_decoded, ticks)
        else:
            ac_false, ac_wrong, ac_timing = score_ac_by_hex(ac_truth, ac_decoded)
        ac = {"replies": len(ac_truth), "decoded": len(ac_decoded), "matched": sum(1 for a in ac_truth if a["_hit"]),
              "false": len(ac_false), "wrong": len(ac_wrong), "timing": ac_timing, "bands": [], "mode": {},
              "false_per_min": len(ac_false) / seconds * 60 if seconds > 0 else None}
        for lo, hi in BANDS:
            band = [a for a in ac_truth if lo <= a["adsb:snr_db"] < hi]
            row = {"snr_lo": lo, "snr_hi": hi}
            for kind in ("clean", "collided"):
                k, n, _ = rate([a for a in band if a["adsb:collided"] == (kind == "collided")])
                row[kind] = [k, n]
            ac["bands"].append(row)
        for mode in ("A", "C"):
            k, n, _ = rate([a for a in ac_truth if a.get("adsb:mode_ac") == mode])
            ac["mode"][mode] = [k, n]

    summary = {"messages": len(truth), "decoded": len(decoded), "false": false, "matching": "time" if timed else "hex",
               "false_by_df": dict(false_by_df), "timing": timing, "bands": [], "df": {}, "mode_ac": ac}
    for lo, hi in BANDS:
        band = [a for a in truth if lo <= a["adsb:snr_db"] < hi]
        row = {"snr_lo": lo, "snr_hi": hi}
        for kind in ("clean", "collided"):
            rows = [a for a in band if a["adsb:collided"] == (kind == "collided")]
            k, n, _ = rate(rows)
            row[kind] = [k, n]
        summary["bands"].append(row)
    by_df = collections.defaultdict(list)
    for a in truth:
        by_df[a["core:label"]].append(a)
    for df, rows in sorted(by_df.items()):
        k, n, _ = rate(rows)
        summary["df"][df] = [k, n]

    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        k, n, r = rate(truth)
        print(f"truth {n} messages, decoded {summary['decoded']} ({k} matched, {false} false; matched by "
              f"{summary['matching']})")
        if timing:
            print(f"timing: decoder offset {timing['offset_samples']:.2f} samples, error p50 "
                  f"{timing['error_p50']:+.3f}, max |error| {timing['error_abs_max']:.3f} samples; "
              f"{timing['late_copies']} of the false decodes are late copies of real messages (echoes)")
        print(f"{'SNR dB':>12}  {'clean':>16}  {'collided':>16}")
        for row in summary["bands"]:
            lo, hi = row["snr_lo"], row["snr_hi"]
            label = f"< {hi}" if lo == -99 else (f">= {lo}" if hi == 99 else f"{lo}-{hi}")
            cells = []
            for kind in ("clean", "collided"):
                k, n = row[kind]
                cells.append(f"{k:>6}/{n:<6} {100 * k / n:5.1f}%" if n else f"{'-':>16}")
            print(f"{label:>12}  {cells[0]:>16}  {cells[1]:>16}")
        print("by format: " + ", ".join(f"{df} {k}/{n}" for df, (k, n) in summary["df"].items()))
        if false:
            print("false decodes by format: " + ", ".join(f"{df} {n}" for df, n in sorted(false_by_df.items())))
        if ac:
            fpm = f", {ac['false_per_min']:.1f} false/min" if ac["false_per_min"] is not None else ""
            print(f"Mode A/C: truth {ac['replies']} replies, decoded {ac['decoded']} ({ac['matched']} matched, "
                  f"{ac['wrong']} wrong code, {ac['false']} false{fpm})")
            if ac["timing"] and ac["timing"]["matched"]:
                t = ac["timing"]
                print(f"  timing: decoder offset {t['offset_samples']:.2f} samples, error p50 {t['error_p50']:+.3f}, "
                      f"max |error| {t['error_abs_max']:.3f} samples")
            for row in ac["bands"]:
                lo, hi = row["snr_lo"], row["snr_hi"]
                label = f"< {hi}" if lo == -99 else (f">= {lo}" if hi == 99 else f"{lo}-{hi}")
                cells = []
                for kind in ("clean", "collided"):
                    k, n = row[kind]
                    cells.append(f"{k:>6}/{n:<6} {100 * k / n:5.1f}%" if n else f"{'-':>16}")
                print(f"  {label:>10}  {cells[0]:>16}  {cells[1]:>16}")
            print("  by mode: " + ", ".join(f"{m} {k}/{n}" for m, (k, n) in ac["mode"].items()))

    failed = []
    for rule in args.require:
        m = re.fullmatch(r"false<=(\d+)", rule)
        if m:
            if false > int(m.group(1)):
                failed.append(f"{rule}: {false} false decodes")
            continue
        m = re.fullmatch(r"false17<=(\d+)", rule)
        if m:
            if false_by_df.get("DF17", 0) > int(m.group(1)):
                failed.append(f"{rule}: {false_by_df['DF17']} false DF17 decodes")
            continue
        m = re.fullmatch(r"acwrong<=([\d.]+)", rule)
        if m:
            found = ac["matched"] + ac["wrong"] if ac else 0
            share = ac["wrong"] / found if found else 0.0
            if not ac or share > float(m.group(1)):
                failed.append(f"{rule}: {ac and ac['wrong']} wrong of {found}")
            continue
        m = re.fullmatch(r"acfalse<=(\d+)", rule)
        if m:
            if not ac or ac["false"] > int(m.group(1)):
                failed.append(f"{rule}: {ac and ac['false']} false Mode A/C")
            continue
        m = re.fullmatch(r"acfalsepm<=([\d.]+)", rule)
        if m:
            if not ac or ac["false_per_min"] is None or ac["false_per_min"] > float(m.group(1)):
                failed.append(f"{rule}: {ac and ac['false_per_min']} false Mode A/C per minute")
            continue
        m = re.fullmatch(r"acclean>=(-?[\d.]+)(?:<(-?[\d.]+))?:([\d.]+)", rule)
        if m:
            hi = float(m.group(2)) if m.group(2) else None
            k, n, r = rate(select(ac_truth, "clean", float(m.group(1)), hi))
            if n == 0 or r < float(m.group(3)):
                failed.append(f"{rule}: {k}/{n}")
            continue
        m = re.fullmatch(r"timing<=([\d.]+)", rule)
        if m:
            if not timing or timing["error_abs_max"] is None or timing["error_abs_max"] > float(m.group(1)):
                failed.append(f"{rule}: {timing and timing['error_abs_max']}")
            continue
        m = re.fullmatch(r"(clean|collided|all)>=(-?[\d.]+)(?:<(-?[\d.]+))?:([\d.]+)", rule)
        if not m:
            print(f"bad --require rule: {rule}", file=sys.stderr)
            return 2
        hi = float(m.group(3)) if m.group(3) else None
        k, n, r = rate(select(truth, m.group(1), float(m.group(2)), hi))
        if n == 0 or r < float(m.group(4)):
            failed.append(f"{rule}: {k}/{n}")
    for f in failed:
        print(f"FAIL {f}")
    if args.require and not failed:
        print(f"OK: {len(args.require)} requirement(s) met")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
