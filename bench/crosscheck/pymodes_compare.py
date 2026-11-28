#!/usr/bin/env python3
"""Compare our decoder with pyModeS, frame by frame.

    python pymodes_compare.py OURS.jsonl [--surface-ref LAT,LON] [--json OUT]
                              [--samples N] [--limit N]

OURS.jsonl is the output of `adsb-decode REC.adsbrec` (core/tools). Each
frame's hex is decoded again by pyModeS's PipeDecoder (pip install
pyModeS; written against 3.7) with the same timestamp, and every field
both decoders can produce is compared:

  agree       both have it and the values match (within the tolerance below)
  differ      both have it, the values do not match
  only ours   we have it, pyModeS has none (key missing or None)
  only pyms   pyModeS has it, we have none

pyModeS's PipeDecoder keeps per-aircraft state (Comm-B inference, DF20
address checks against known aircraft, velocity and position consistency
checks), so some "only" counts are policy, not decoding; the report says
which fields are affected. Positions are compared per frame once pyModeS
can no longer change them: it holds the first positions of an aircraft
until three agree and fills them in afterwards, so each CPR frame is
judged 120 s of stream time later (and at the end after flush()).

The output lists, per field, the counts and up to --samples example frames
(hex, ours, pyModeS) for each kind of mismatch, so each one can be checked
against the standard by hand.
"""

from __future__ import annotations

import argparse
import collections
import heapq
import json
import math
import sys

try:
    from pyModeS import PipeDecoder
    import pyModeS
    from pyModeS.position import airborne_position_with_ref, surface_position_with_ref
except ImportError:  # pragma: no cover
    sys.exit("pymodes_compare: pyModeS is not installed (pip install pyModeS==3.7.0)")

POSITION_LAG_S = 120.0


def close(tol):
    return lambda a, b: abs(float(a) - float(b)) <= tol


def angle_close(tol):
    def f(a, b):
        d = abs(float(a) - float(b)) % 360.0
        return min(d, 360.0 - d) <= tol
    return f


def eq(a, b):
    return a == b


def is_adsb(o):
    """DF17, or DF18 with a control field that carries an ADS-B ME field."""
    return o.get("df") == 17 or (o.get("df") == 18 and o.get("ca") in (0, 1, 2, 5, 6))


def df18_not_adsb(o):
    """DF18 CF 3 (TIS-B coarse), 4 (TIS-B/ADS-R management), 7 (reserved): no
    ADS-B ME field (DO-260B 2.2.3.2.1.4). We decode nothing from them;
    pyModeS decodes the ME field as if it were ADS-B."""
    return o.get("df") == 18 and o.get("ca") not in (0, 1, 2, 5, 6)


def tc(o):
    return o.get("tc", -1) if is_adsb(o) else -1


def airborne_pos(o):
    return 9 <= tc(o) <= 18 or 20 <= tc(o) <= 22


def surface_pos(o):
    return 5 <= tc(o) <= 8


def ident(o):
    return 1 <= tc(o) <= 4


def velocity(o):
    return tc(o) == 19


def op_status(o):
    return tc(o) == 31


def target_state(o):
    return tc(o) == 29


def ours_ground(o):
    g = o.get("ground")
    return None if g is None else bool(g)


def pyms_reply_ground(r, o):
    """pyModeS's air/ground of a DF0/4/5/16/20/21 reply (VS or FS field)."""
    df = o.get("df")
    if df in (0, 16):
        vs = r.get("vertical_status")
        return None if vs is None else vs != "airborne"
    fs = r.get("flight_status")
    if fs in (1, 3):
        return True
    if fs in (0, 2):
        return False
    return None  # FS 4/5: either; 6/7 reserved


def replies(o):
    return o.get("df") in (0, 4, 5, 16, 20, 21)


def says_ground(r, o):
    return replies(o) and pyms_reply_ground(r, o) is True


def says_airborne(r, o):
    return replies(o) and pyms_reply_ground(r, o) is False


# (name, applies(o) or applies(o, r), ours(o), pyms(r, o), match)
FIELDS = [
    ("df", lambda o: True, lambda o: o.get("df"), lambda r, o: r.get("df"), eq),
    ("icao", lambda o: True, lambda o: o.get("icao"), lambda r, o: r.get("icao"), eq),
    ("typecode", is_adsb, lambda o: o.get("tc"), lambda r, o: r.get("typecode"), eq),
    ("altitude (baro)", lambda o: not (20 <= tc(o) <= 22),
     lambda o: o.get("alt") if o.get("gnss") == 0 else None, lambda r, o: r.get("altitude"), eq),
    ("altitude (GNSS, TC 20-22)", lambda o: 20 <= tc(o) <= 22,
     lambda o: o.get("alt"), lambda r, o: r.get("altitude"), eq),
    # Replies: VS 1 / FS 1,3 mean on ground. VS 0 / FS 0,2 mean "airborne" to
    # pyModeS; our decoder gives no air/ground for them (readsb's rule: a
    # transponder that cannot tell reports airborne), so "only pyms" there is
    # by design.
    ("air/ground: reply says on ground", says_ground, ours_ground, pyms_reply_ground, eq),
    ("air/ground: reply says airborne", says_airborne, ours_ground, pyms_reply_ground, eq),
    ("squawk", lambda o: True, lambda o: o.get("sq"), lambda r, o: r.get("squawk"), eq),
    ("callsign (ADS-B)", is_adsb, lambda o: o.get("cs"), lambda r, o: r.get("callsign"), eq),
    ("category", ident, lambda o: int(o["cat"][1], 16) if "cat" in o else None,
     lambda r, o: r.get("category"), eq),
    ("emergency", lambda o: tc(o) == 28, lambda o: o.get("emerg"), lambda r, o: r.get("emergency_state"), eq),
    ("CPR lat/lon/format", lambda o: airborne_pos(o) or surface_pos(o),
     lambda o: tuple(o["cpr"][:3]) if "cpr" in o else None,
     lambda r, o: (r["cpr_lat"], r["cpr_lon"], r["cpr_format"]) if r.get("cpr_lat") is not None else None, eq),
    ("NIC supplement B", airborne_pos, lambda o: o.get("nicb"), lambda r, o: r.get("nic_b"), eq),
    ("NUCp", lambda o: airborne_pos(o), lambda o: o.get("nuc"), lambda r, o: r.get("nuc_p"), eq),
    # Velocity (TC 19) and surface movement. pyModeS reports airborne ground
    # speed and airspeed in whole knots (int()), so 1 kt is allowed there.
    ("ground speed (airborne)", velocity, lambda o: o.get("gs"), lambda r, o: r.get("groundspeed"), close(1.0)),
    ("ground speed (surface)", surface_pos, lambda o: o.get("gs"), lambda r, o: r.get("groundspeed"), close(1e-3)),
    ("track", lambda o: velocity(o) or surface_pos(o), lambda o: o.get("trk"), lambda r, o: r.get("track"),
     angle_close(1e-3)),
    ("vertical rate", velocity, lambda o: o.get("vr"), lambda r, o: r.get("vertical_rate"), eq),
    ("vertical rate source", velocity, lambda o: None if "vr_gnss" not in o else ("GNSS" if o["vr_gnss"] else "BARO"),
     lambda r, o: r.get("vr_source"), eq),
    ("GNSS - baro", velocity, lambda o: o.get("gnss_diff"), lambda r, o: r.get("geo_minus_baro"), eq),
    ("airspeed", velocity, lambda o: o.get("ias", o.get("tas")), lambda r, o: r.get("airspeed"), close(1.0)),
    ("airspeed type", velocity, lambda o: "TAS" if "tas" in o else "IAS" if "ias" in o else None,
     lambda r, o: r.get("airspeed_type"), eq),
    ("heading (velocity)", velocity, lambda o: o.get("hdg"), lambda r, o: r.get("heading"), angle_close(1e-3)),
    # TC 31
    ("TC31 version", op_status, lambda o: o.get("op", {}).get("version"), lambda r, o: r.get("version"), eq),
    ("TC31 NIC-A", op_status, lambda o: o.get("op", {}).get("nica"), lambda r, o: r.get("nic_supplement_a"), eq),
    ("TC31 NACp", op_status, lambda o: o.get("op", {}).get("nacp"), lambda r, o: r.get("nac_p"), eq),
    ("TC31 SIL", op_status, lambda o: o.get("op", {}).get("sil"), lambda r, o: r.get("sil"), eq),
    ("TC31 NIC-baro", op_status, lambda o: o.get("op", {}).get("nicbaro"), lambda r, o: r.get("nic_baro"), eq),
    ("TC31 HRD (true north)", op_status, lambda o: o.get("op", {}).get("hrd_true"),
     lambda r, o: None if r.get("hrd") is None else int(r["hrd"] == 0), eq),
    # TC 29
    ("TC29 selected altitude", target_state, lambda o: o.get("tgt", {}).get("alt"),
     lambda r, o: r.get("selected_altitude"), eq),
    ("TC29 altitude source", lambda o: target_state(o) and "alt" in o.get("tgt", {}),
     lambda o: None if "fms" not in o.get("tgt", {}) else ("FMS" if o["tgt"]["fms"] else "MCP/FCU"),
     lambda r, o: r.get("selected_altitude_source"), eq),
    ("TC29 baro setting", target_state, lambda o: o.get("tgt", {}).get("baro"),
     lambda r, o: r.get("baro_pressure_setting"), close(0.05)),
    ("TC29 selected heading", target_state, lambda o: o.get("tgt", {}).get("hdg"),
     lambda r, o: r.get("selected_heading"), angle_close(1e-3)),
    ("TC29 NACp", target_state, lambda o: o.get("tgt", {}).get("nacp"), lambda r, o: r.get("nac_p"), eq),
    ("TC29 SIL", target_state, lambda o: o.get("tgt", {}).get("sil"), lambda r, o: r.get("sil"), eq),
    ("TC29 NIC-baro", target_state, lambda o: o.get("tgt", {}).get("nicbaro"), lambda r, o: r.get("nic_baro"), eq),
    ("TC29 TCAS operational", target_state, lambda o: o.get("tgt", {}).get("tcas"),
     lambda r, o: None if r.get("tcas_operational") is None else int(bool(r["tcas_operational"])), eq),
    ("TC29 modes (AP VNAV hold app LNAV)", target_state,
     lambda o: tuple(o["tgt"][k] for k in ("ap", "vnav", "hold", "app", "lnav")) if "ap" in o.get("tgt", {}) else None,
     lambda r, o: None if r.get("autopilot") is None else
     tuple(int(bool(r[k])) for k in ("autopilot", "vnav_mode", "altitude_hold_mode", "approach_mode", "lnav_mode")),
     eq),
    # Comm-B. We name a register only when exactly one fits; pyModeS also uses
    # what it knows of the aircraft. So "only" counts are expected both ways.
    ("Comm-B register", lambda o: o.get("df") in (20, 21),
     lambda o: None if o.get("commb", {}).get("bds", "0,0") == "0,0" else o["commb"]["bds"],
     lambda r, o: r.get("bds"), eq),
    ("Comm-B callsign (BDS 2,0)", lambda o: o.get("df") in (20, 21), lambda o: o.get("commb", {}).get("cs"),
     lambda r, o: r.get("callsign"), eq),
]


def haversine_m(a, b):
    lat1, lon1, lat2, lon2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = math.sin((lat2 - lat1) / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin((lon2 - lon1) / 2) ** 2
    return 2 * 6371008.8 * math.asin(min(1.0, math.sqrt(h)))


def short(o):
    return {k: v for k, v in o.items() if k not in ("i", "hex")}


class Tally:
    def __init__(self, samples):
        self.samples = samples
        self.counts = collections.OrderedDict((f[0], collections.Counter()) for f in FIELDS)
        self.examples = collections.defaultdict(list)
        # A difference that repeats is usually one cause: group by (field, ours, pyms).
        self.differ_values = collections.defaultdict(collections.Counter)

    def add(self, name, kind, o, r, a=None, b=None):
        self.counts[name][kind] += 1
        if kind == "differ":
            self.differ_values[name][(repr(a), repr(b))] += 1
        if kind != "agree" and len(self.examples[(name, kind)]) < self.samples:
            self.examples[(name, kind)].append({"hex": o["hex"], "t": o["t"], "ours": short(o),
                                                 "pyms": {k: v for k, v in r.items() if v is not None}})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ours")
    ap.add_argument("--surface-ref", help="LAT,LON for pyModeS surface positions (the receiver)")
    ap.add_argument("--json", help="write the counts and examples here")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    ref = None
    if args.surface_ref:
        lat, lon = (float(x) for x in args.surface_ref.split(","))
        ref = (lat, lon)
    pipe = PipeDecoder(surface_ref=ref)
    tally = Tally(args.samples)
    status = collections.Counter()
    pyms_errors = collections.Counter()

    pos_counts = collections.Counter()
    exact_dist = []   # ours vs pyModeS's stateless decode of the same frame with ours as reference
    exact_far = []
    own_dist = []     # ours vs PipeDecoder where PipeDecoder's position is this frame's own
    largest = []      # heap of the largest ours-vs-PipeDecoder differences
    our_range = []    # distance of our positions from the receiver (--surface-ref)
    pos_dist = []
    pos_far = []
    pos_examples = collections.defaultdict(list)
    pending = collections.deque()  # (t, o, r) of CPR frames waiting for pyModeS to settle

    def frame_decode(o, ref):
        lat_raw, lon_raw, fmt = o["cpr"][:3]
        f = surface_position_with_ref if surface_pos(o) else airborne_position_with_ref
        return f(fmt, lat_raw, lon_raw, ref[0], ref[1])

    def judge_position(o, r):
        ours = o.get("pos")
        lat, lon = r.get("latitude"), r.get("longitude")
        theirs = (lat, lon) if lat is not None and lon is not None else None
        kind = "surface" if surface_pos(o) else "airborne"
        if ours and ref:
            our_range.append(haversine_m(ours, ref))
        if ours:
            # Same zone as ours: the frame's own position by pyModeS's formula.
            d = haversine_m(ours, frame_decode(o, ours))
            exact_dist.append(d)
            if d > 1.0 and len(exact_far) < 50:
                exact_far.append({"d_m": d, "hex": o["hex"], "ours": ours})
        if ours and theirs:
            d = haversine_m(ours, theirs)
            pos_counts[(kind, "both")] += 1
            pos_dist.append(d)
            # PipeDecoder also hands a pair's position to the pair's earlier
            # frames; only where its position is this frame's own does a
            # difference mean the two chose different zones.
            own = haversine_m(theirs, frame_decode(o, theirs)) < 1.0
            if own:
                own_dist.append(d)
            e = (d, o["i"], {"d_m": round(d, 1), "own": own, "hex": o["hex"], "t": o["t"], "icao": o.get("icao"),
                             "ours": ours, "pyms": theirs})
            if len(largest) < 20:
                heapq.heappush(largest, e)
            elif d > largest[0][0]:
                heapq.heapreplace(largest, e)
            if d > 50 and len(pos_far) < 200:
                pos_far.append({"d_m": round(d, 1), "hex": o["hex"], "t": o["t"], "icao": o.get("icao"),
                                "ours": ours, "pyms": theirs})
        elif ours or theirs:
            which = "only ours" if ours else "only pyms"
            pos_counts[(kind, which)] += 1
            ex = pos_examples[(kind, which)]
            if len(ex) < args.samples:
                ex.append({"hex": o["hex"], "t": o["t"], "icao": o.get("icao"), "ours": ours, "pyms": theirs})
        else:
            pos_counts[(kind, "neither")] += 1

    n = 0
    with open(args.ours) as f:
        for line in f:
            o = json.loads(line)
            n += 1
            if args.limit and n > args.limit:
                break
            t = float(o["t"])
            status[o["st"]] += 1
            try:
                r = pipe.decode(o["hex"], timestamp=t)
            except Exception as e:  # noqa: BLE001  pyModeS rejects some frames outright
                pyms_errors[(o["st"], type(e).__name__)] += 1
                continue
            if o["st"] not in ("ok", "address-parity"):
                # Frames we refuse: say what pyModeS made of them.
                status[("refused by us", o["st"], "pyms crc_valid", r.get("crc_valid"))] += 1
                continue
            if df18_not_adsb(o):
                status["DF18 CF 3/4/7 (no ADS-B ME): not compared"] += 1
                continue
            for name, applies, ours_of, pyms_of, match in FIELDS:
                if not (applies(o, r) if applies.__code__.co_argcount == 2 else applies(o)):
                    continue
                a = ours_of(o)
                b = pyms_of(r, o)
                if a is None and b is None:
                    continue
                if a is None:
                    tally.add(name, "only pyms", o, r)
                elif b is None:
                    tally.add(name, "only ours", o, r)
                elif match(a, b):
                    tally.add(name, "agree", o, r)
                else:
                    tally.add(name, "differ", o, r, a, b)
            if "cpr" in o:
                pending.append((t, o, r))
            while pending and t - pending[0][0] > POSITION_LAG_S:
                _, po, pr = pending.popleft()
                judge_position(po, pr)

    pipe.flush()
    while pending:
        _, po, pr = pending.popleft()
        judge_position(po, pr)

    # ---- report ----------------------------------------------------------
    out = sys.stdout
    print(f"pyModeS {pyModeS.__version__}; {n} frames", file=out)
    print("our status: " + ", ".join(f"{k} {v}" for k, v in status.items() if isinstance(k, str)), file=out)
    for k, v in status.items():
        if not isinstance(k, str):
            print(f"  {k[0]} ({k[1]}), pyModeS crc_valid={k[3]}: {v}", file=out)
    if pyms_errors:
        print("pyModeS raised: " + ", ".join(f"{k} {v}" for k, v in pyms_errors.items()), file=out)
    print(file=out)
    print(f"{'field':38s} {'compared':>9s} {'agree':>9s} {'differ':>7s} {'only ours':>9s} {'only pyms':>9s}", file=out)
    for name, c in tally.counts.items():
        tot = sum(c.values())
        if tot == 0:
            continue
        print(f"{name:38s} {tot:9d} {c['agree']:9d} {c['differ']:7d} {c['only ours']:9d} {c['only pyms']:9d}", file=out)
    print(file=out)
    for name, vals in tally.differ_values.items():
        top = ", ".join(f"ours {a} vs pyms {b} x{k}" for (a, b), k in vals.most_common(6))
        print(f"differ  {name}: {top}", file=out)
    print(file=out)
    print("positions (per CPR frame, after pyModeS settled):", file=out)
    for kind in ("airborne", "surface"):
        c = {w: pos_counts[(kind, w)] for w in ("both", "only ours", "only pyms", "neither")}
        print(f"  {kind:9s} both {c['both']}, only ours {c['only ours']}, only pyms {c['only pyms']}, "
              f"neither {c['neither']}", file=out)
    def dist_line(label, v):
        if not v:
            return
        s = sorted(v)
        q = lambda p: s[min(len(s) - 1, int(p * len(s)))]
        print(f"  {label}: n {len(s)}, p50 {q(0.5):.3f} m, p99 {q(0.99):.3f} m, max {s[-1]:.3f} m, "
              f"> 1 m {sum(d > 1 for d in s)}, > 1 km {sum(d > 1000 for d in s)}", file=out)
    dist_line("ours vs pyModeS formula, same frame, our zone", exact_dist)
    dist_line("ours vs PipeDecoder, its position is the frame's own", own_dist)
    dist_line("ours vs PipeDecoder, all frames with both", pos_dist)
    if our_range:
        s_r = sorted(our_range)
        print(f"  our positions from the receiver: max {s_r[-1] / 1000:.1f} km, "
              f"> 500 km {sum(d > 500e3 for d in s_r)}", file=out)
    for d, _, e in sorted(largest, reverse=True)[:5]:
        print(f"  largest: {e['d_m']} m {'own' if e['own'] else 'inherited'} {e['icao']} {e['hex']} "
              f"ours {e['ours'][:2]} pyms {[round(x, 5) for x in e['pyms']]}", file=out)
    print(f"pyModeS stats: {dict(pipe.stats)}", file=out)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({
                "pymodes": pyModeS.__version__, "frames": n,
                "status": {str(k): v for k, v in status.items()},
                "pyms_errors": {str(k): v for k, v in pyms_errors.items()},
                "fields": {k: dict(v) for k, v in tally.counts.items() if sum(v.values())},
                "differ_values": {k: [[a, b, c] for (a, b), c in v.most_common(20)]
                                  for k, v in tally.differ_values.items()},
                "examples": {f"{k[0]} / {k[1]}": v for k, v in tally.examples.items()},
                "positions": {f"{k[0]} {k[1]}": v for k, v in pos_counts.items()},
                "position_distance_m": {"n": len(pos_dist), "max": max(pos_dist) if pos_dist else None},
                "position_exact_far": exact_far,
                "position_largest": [e for _, _, e in sorted(largest, reverse=True)],
                "position_own_far": sorted((d for d in own_dist if d > 1.0), reverse=True)[:50],
                "position_far": pos_far,
                "position_examples": {f"{k[0]} / {k[1]}": v for k, v in pos_examples.items()},
                "pyms_stats": dict(pipe.stats),
            }, f, indent=1)


if __name__ == "__main__":
    main()
