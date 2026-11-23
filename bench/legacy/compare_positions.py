#!/usr/bin/env python3
"""Position-by-position comparison of the new decoder with readsb.

    compare_positions.py DIR

DIR holds (see compare-positions.sh, which makes them):
    t.adsbrec    the recording both decoders received
    new.bin      adsb-replay --out of it (TrackUpdate stream)
    sbs.csv      readsb's SBS output, one line per message, "t_ns,line"
    feed.json    feeder.py's timing (maps readsb's wall clock to recording time)

Every position the new decoder emits is matched to a readsb position of the
same aircraft with the same coordinates (SBS prints 5-6 decimals, so within
2e-5 degrees, about 2 m), and the rest on both sides are explained:
    ours only     by message type: surface positions are SBS MSG,2, airborne MSG,3
    readsb only   in order of the first rule that applies:
                  - next to a corrupted frame of that aircraft (readsb repairs
                    single-bit errors by default; the new decoder rejects them);
                  - a corrupted frame of that aircraft in the 10 s before (its
                    repair gave readsb the other half of an even/odd CPR pair);
                  - the same position printed more often (readsb repeats an
                    unchanged position; the track engine sends only changes);
                  - before the new decoder's first position of that aircraft
                    (a track's first fix);
                  - unexplained
Prints a summary, or JSON with --json.
"""

import argparse
import collections
import json
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze import load_new_updates  # noqa: E402

MATCH_DEG = 2e-5        # SBS rounding
CORRUPT_WINDOW_NS = 300_000_000  # readsb's output can trail the frame by its 50 ms buffering and more
CPR_PAIR_NS = 10_000_000_000     # even/odd frames pair within 10 s


def crc24_syndrome(frame):
    c = 0
    for x in frame[:-3]:
        c ^= x << 16
        for _ in range(8):
            c = ((c << 1) ^ 0x1FFF409) if c & 0x800000 else (c << 1)
    return (c & 0xFFFFFF) ^ int.from_bytes(frame[-3:], "big")


def load_records(path):
    """{(icao hex, rx_ns): payload} for 112-bit frames, and [(rx_ns, icao int)] of corrupted ones."""
    data = Path(path).read_bytes()
    if data[:8] != b"ADSBREC1":
        sys.exit(f"{path}: not an .adsbrec file")
    end = len(data) - 24 if data[-24:-16] == b"ADSBEND1" else len(data)
    recs, corrupt = {}, []
    pos = 64
    while pos + 20 <= end:
        rx, _rts, _line, _typ, _sig, ln = struct.unpack_from("<QQBBBB", data, pos)
        p = data[pos + 20:pos + 20 + ln]
        pos += 20 + ln
        if ln != 14:
            continue
        icao = int.from_bytes(p[1:4], "big")
        recs.setdefault((f"{icao:06X}", rx), p)
        if crc24_syndrome(p) != 0:
            corrupt.append((rx, icao))
    return recs, corrupt


def load_sbs_positions(path, meta):
    """{hex: [(recording-time ns, lat, lon, msg type)]} for SBS MSG,2 and MSG,3 lines with a position."""
    out = collections.defaultdict(list)
    with open(path) as f:
        next(f, None)
        for line in f:
            t_s, _, msg = line.rstrip("\n").partition(",")
            p = msg.split(",")
            if len(p) < 18 or p[0] != "MSG" or p[1] not in ("2", "3") or not p[14] or not p[15]:
                continue
            t = meta["first_rx_ns"] + int((int(t_s) - meta["t0_wall_ns"]) * meta.get("speed", 1.0))
            out[p[4].strip().upper()].append((t, float(p[14]), float(p[15]), p[1]))
    return out


def metres(la1, lo1, la2, lo2):
    return math.hypot((la1 - la2) * 111_195, (lo1 - lo2) * 111_195 * math.cos(math.radians(la1)))


def compare(d):
    d = Path(d)
    meta = json.loads((d / "feed.json").read_text())
    new, _ = load_new_updates(d / "new.bin")
    recs, corrupt = load_records(d / "t.adsbrec")
    sbs = load_sbs_positions(d / "sbs.csv", meta)

    ours = collections.defaultdict(list)
    for t, hx, la, lo, _alt in new:
        ours[hx].append((t, la, lo))

    ours_only, theirs_only = [], []
    for hx in set(ours) | set(sbs):
        pool = list(sbs.get(hx, []))
        for t, la, lo in ours.get(hx, []):
            k = next((i for i, q in enumerate(pool) if abs(q[1] - la) < MATCH_DEG and abs(q[2] - lo) < MATCH_DEG), None)
            if k is None:
                ours_only.append((hx, t, la, lo))
            else:
                pool.pop(k)
        theirs_only += [(hx, *q) for q in pool]

    result = {
        "ours": len(new),
        "readsb_msg3": sum(1 for v in sbs.values() for q in v if q[3] == "3"),
        "readsb_msg2": sum(1 for v in sbs.values() for q in v if q[3] == "2"),
        "matched": len(new) - len(ours_only),
        "corrupted_frames": len(corrupt),
    }

    kinds = collections.Counter()
    for hx, t, _la, _lo in ours_only:
        p = recs.get((hx, t))
        tc = p[4] >> 3 if p is not None else None
        kinds["surface (TC 5-8)" if tc is not None and 5 <= tc <= 8 else
              "airborne" if tc is not None and 9 <= tc <= 22 else "no frame found"] += 1
    result["ours_only"] = {"total": len(ours_only), **kinds}

    near = collections.defaultdict(list)
    for rx, icao in corrupt:
        near[icao >> 12].append((rx, icao))  # coarse bucket; one-bit-off addresses checked below

    def corrupt_of(hx):
        h = int(hx, 16)
        cands = [c for b in {h >> 12, *[(h ^ (1 << i)) >> 12 for i in range(24)]} for c in near.get(b, [])]
        return [rx for rx, ic in cands if bin(ic ^ h).count("1") <= 1]

    why = collections.Counter()
    for hx, t, la, lo, _m in theirs_only:
        bad = corrupt_of(hx)
        mine = ours.get(hx, [])
        before = [q for q in mine if q[0] <= t + 50_000_000]
        if any(abs(t - rx) < CORRUPT_WINDOW_NS for rx in bad):
            why["next to a corrupted frame of that aircraft (readsb repaired it)"] += 1
        elif any(0 < t - rx < CPR_PAIR_NS for rx in bad):
            why["a repaired frame completed readsb's CPR pair"] += 1
        elif before and metres(la, lo, before[-1][1], before[-1][2]) < 1.0:
            why["the same position, printed more often by readsb"] += 1
        elif not before:
            why["before our first position of that aircraft"] += 1
        else:
            why["unexplained"] += 1
    result["readsb_only"] = {"total": len(theirs_only), **why}
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    r = compare(a.dir)
    if a.json:
        print(json.dumps(r, indent=2))
        return
    sb = r["readsb_msg3"] + r["readsb_msg2"]
    print(f"new decoder positions      {r['ours']:>8,}")
    print(f"readsb SBS MSG,3 (airborne) {r['readsb_msg3']:>7,}   -> ours / MSG,3 = {r['ours'] / max(1, r['readsb_msg3']):.3f}")
    print(f"readsb SBS MSG,2 (surface)  {r['readsb_msg2']:>7,}   -> ours / (MSG,2 + MSG,3) = {r['ours'] / max(1, sb):.4f}")
    print(f"matched one to one          {r['matched']:>7,}")
    print(f"only ours                   {r['ours_only']['total']:>7,}")
    for k, v in r["ours_only"].items():
        if k != "total":
            print(f"    {k}: {v:,}")
    print(f"only readsb                 {r['readsb_only']['total']:>7,}   (corrupted frames in the recording: {r['corrupted_frames']:,})")
    for k, v in r["readsb_only"].items():
        if k != "total":
            print(f"    {k}: {v:,}")


if __name__ == "__main__":
    main()
