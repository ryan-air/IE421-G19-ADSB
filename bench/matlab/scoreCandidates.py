#!/usr/bin/env python3
"""Attribute DF17 misses to preamble/timing or to bit/format/CRC rejection."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter
import csv
import json


def classify(metadata, score, candidates):
    rate = metadata["global"]["core:sample_rate"]
    offset = score["timing"]["offset_samples"]
    tolerance = 4  # same window as iq-score.py, measured in input samples
    candidates.sort(key=lambda row: float(row["time_s"]))
    positions = [float(row["time_s"]) * rate - offset for row in candidates]
    truth = [row for row in metadata["annotations"] if row["core:label"] == "DF17"]
    edges = []
    for truth_index, row in enumerate(truth):
        position = row["adsb:sample_pos"]
        for index in range(bisect_left(positions, position - tolerance),
                           bisect_right(positions, position + tolerance)):
            candidate = candidates[index]
            exact = candidate["hex"] == row["adsb:hex"]
            passed = exact and candidate["crc_error"] == "0"
            edges.append((not passed, not exact, abs(positions[index] - position),
                          truth_index, index))

    assigned = {}
    used = set()
    for _, _, _, truth_index, candidate_index in sorted(edges):
        if truth_index not in assigned and candidate_index not in used:
            assigned[truth_index] = candidates[candidate_index]
            used.add(candidate_index)

    totals = Counter()
    groups = {}
    snr_groups = {}
    for index, row in enumerate(truth):
        candidate = assigned.get(index)
        if candidate is None:
            stage = "no_on_time_preamble"
        elif candidate["hex"] == row["adsb:hex"] and candidate["crc_error"] == "0":
            stage = "matched"
        elif candidate["crc_error"] == "0":
            stage = "different_crc_passing_payload"
        elif candidate["df"] not in ("11", "17"):
            stage = "unsupported_parsed_format"
        else:
            stage = "crc_rejected"
        totals[stage] += 1
        group = "overlapping" if row["adsb:collided"] else "clean"
        groups.setdefault(group, Counter())[stage] += 1
        snr = row["adsb:snr_db"]
        band = "<5 dB" if snr < 5 else "5–15 dB" if snr < 15 else ">=15 dB"
        snr_groups.setdefault(band, Counter())[stage] += 1
    return {"truth": len(truth), "candidates": len(candidates), "stages": totals,
            "by_overlap": groups, "by_snr": snr_groups}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata")
    parser.add_argument("score", help="iq-score.py --df 17 --json output for the diagnostic receiver")
    parser.add_argument("candidates")
    args = parser.parse_args()
    with open(args.metadata) as meta, open(args.score) as score, open(args.candidates, newline="") as candidates:
        metadata = json.load(meta)
        summary = json.load(score)
        if not summary.get("timing") or set(summary["df"]) != {"DF17"}:
            parser.error("expected a time-aligned DF17 score")
        result = classify(metadata, summary, list(csv.DictReader(candidates)))
        if result["stages"]["matched"] != summary["df"]["DF17"][0]:
            parser.error("candidate trace and Beast score disagree on matched DF17 frames")
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
