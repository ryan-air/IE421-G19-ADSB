# Decoder cross-check against pyModeS

Checks our Mode S / ADS-B decoder frame by frame against an independent one,
[pyModeS](https://github.com/junzis/pyModeS) (the library Prof. Lariviere's
`pymodesDecode.py` uses), on real traffic.

```bash
# 1. our decoder, one JSON line per frame (core/tools/adsb_decode.cpp)
core/build/release/tools/adsb-decode REC.adsbrec -o ours.jsonl

# 2. pyModeS on the same frames, field by field (pyModeS is not a project
#    dependency: install it in a virtual environment)
python3 -m venv .venv && .venv/bin/pip install pyModeS==3.7.0
.venv/bin/python bench/crosscheck/pymodes_compare.py ours.jsonl \
    --surface-ref 41.878,-87.647 --json cmp.json > cmp.txt
```

A real capture becomes an `.adsbrec` with `adsb-beast-import` (core/README).
`core/scripts/check-decode.sh BUILD_DIR` runs the same comparison on
synthetic traffic (skipped when pyModeS is not installed).

## What is compared

For every frame both decoders accept, each field either decoder produces:
address, DF, type code, altitude, air/ground, squawk, callsign, category,
emergency state, raw CPR fields, NIC-B, NUCp, velocity (ground speed, track,
vertical rate and its source, GNSS − baro, airspeed and its type, heading),
TC 31 (version, NIC-A, NACp, SIL, NIC-baro, HRD), TC 29 (selected altitude
and its source, baro setting, selected heading, NACp, SIL, NIC-baro, TCAS,
autopilot modes), Comm-B register and BDS 2,0 callsign. Each field is counted
as *agree*, *differ*, *only ours* or *only pyModeS*, with example frames.

Positions need state, so `adsb-decode` also runs the track engine and prints
the position it accepted from each frame. Three comparisons:

1. **Our arithmetic:** pyModeS's own CPR formula on the same frame, with our
   position as the reference (same zone). Should be millimetres.
2. **Zone choice:** PipeDecoder's position where it is that frame's own.
3. **Everything:** all frames where both have a position. PipeDecoder also
   writes a pair's position into the pair's earlier frames, so large numbers
   here are mostly aircraft movement; the largest cases are listed.

## Differences that are expected

| Field | Why |
|---|---|
| DF18 CF 3/4/7 | No ADS-B ME field (TIS-B coarse / management, reserved); we decode nothing, pyModeS decodes the ME field anyway. Not compared. |
| squawk 0000 in TC 28 | All-zero Mode A field means "no code"; we give none. |
| air/ground of replies with VS 0 / FS 0,2 | pyModeS: airborne. We give none (readsb's rule: a transponder that cannot tell says airborne). |
| track at ground speed 0 | Undefined; we give none, pyModeS 0°. |
| vertical-rate source without a rate | We drop both when the rate is "not available". |
| CPR / velocity *only ours* | pyModeS drops these fields from frames its own consistency checks reject (its `altitude_mismatch` / `velocity_mismatch` counters). |
| Comm-B register *only* either side | We name a register only when exactly one fits; pyModeS also uses what it knows about the aircraft. |

## Results (professor's capture, 2026-11-19 evening hour, 2.0 M frames, pyModeS 3.7.0)

* Every field both decoders produce agrees: 0 differences, apart from the
  198 DF18 CF 4 frames above (not compared).
* Positions: our position equals pyModeS's formula on the same frame within
  7 mm on all 216,011 position frames; where PipeDecoder's position is the
  frame's own, within 1 m (no zone disagreements).
* Found a track-engine bug: one CRC-valid frame with corrupt CPR fields made
  phantom positions (A12AA1 near Chicago shown in Antarctica for 5 s; 83
  positions > 500 km from the receiver in day 1). Fixed in
  ryan-air/IE421-G19-ADSB#35 (readsb-style bad-position handling: 83 → 4,
  the 4 from one real aircraft at 505 km; surface frames only pyModeS
  placed 37 → 0).
* pyModeS 3.7.0 itself places some Midway surface aircraft at
  (5.18, −151.95) in the Pacific.
