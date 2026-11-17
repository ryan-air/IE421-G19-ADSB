#!/usr/bin/env python3
"""Replay an .adsbrec recording into readsb's Beast input at real-time pace.

This stands in for the RTL-SDR: readsb receives exactly the frames the
synthetic generator produced, at the times they would have arrived.

Writes a small JSON file mapping recording time to wall-clock time:
    emit_wall_ns(record) = t0_wall_ns + (record.rx_ns - first_rx_ns) / speed
so the analysis can tell when every frame entered the system.
"""

import argparse
import json
import socket
import struct
import sys
import time

HEADER_SIZE = 64
FOOTER_SIZE = 24
REC_FIXED = 20
PAYLOAD_LEN = {ord("1"): 2, ord("2"): 7, ord("3"): 14}


def read_records(path):
    """Yields (rx_ns, radio_ts, line, type, signal, payload) from an .adsbrec file."""
    data = open(path, "rb").read()
    if data[:8] != b"ADSBREC1":
        sys.exit(f"{path}: not an .adsbrec file")
    end = len(data) - FOOTER_SIZE if data[-FOOTER_SIZE:-FOOTER_SIZE + 8] == b"ADSBEND1" else len(data)
    pos = HEADER_SIZE
    while pos + REC_FIXED <= end:
        rx_ns, radio_ts, line, typ, signal, ln = struct.unpack_from("<QQBBBB", data, pos)
        if PAYLOAD_LEN.get(typ) != ln:
            sys.exit(f"{path}: corrupt record at byte {pos}")
        yield rx_ns, radio_ts, line, typ, signal, data[pos + REC_FIXED:pos + REC_FIXED + ln]
        pos += REC_FIXED + ln


def beast_frame(radio_ts, typ, signal, payload):
    body = (radio_ts & 0xFFFFFFFFFFFF).to_bytes(6, "big") + bytes([signal]) + payload
    return b"\x1a" + bytes([typ]) + body.replace(b"\x1a", b"\x1a\x1a")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("recording")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=30004)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--line", type=int, default=0, help="receiver line to send (one Beast feed per receiver)")
    ap.add_argument("--meta", required=True, help="where to write the timing JSON")
    args = ap.parse_args()

    records = [r for r in read_records(args.recording) if r[2] == args.line]
    if not records:
        sys.exit("no records for that line")

    for attempt in range(100):
        try:
            sock = socket.create_connection((args.host, args.port), timeout=5)
            break
        except OSError:
            time.sleep(0.1)
    else:
        sys.exit(f"cannot connect to {args.host}:{args.port}")
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    first_rx = records[0][0]
    t0 = time.time_ns() + 200_000_000  # start 200 ms from now
    meta = {"t0_wall_ns": t0, "first_rx_ns": first_rx, "speed": args.speed,
            "records": len(records), "line": args.line}
    with open(args.meta, "w") as f:
        json.dump(meta, f)

    sent = 0
    late_ms_max = 0.0
    i = 0
    n = len(records)
    while i < n:
        due = t0 + int((records[i][0] - first_rx) / args.speed)
        now = time.time_ns()
        if due > now:
            time.sleep(min((due - now) / 1e9, 0.002))
            continue
        late_ms_max = max(late_ms_max, (now - due) / 1e6)
        # Send every record that is due now in one write.
        batch = []
        while i < n and t0 + int((records[i][0] - first_rx) / args.speed) <= now:
            rx, rts, _line, typ, sig, payload = records[i]
            batch.append(beast_frame(rts, typ, sig, payload))
            i += 1
        sock.sendall(b"".join(batch))
        sent += len(batch)

    meta["sent"] = sent
    meta["max_lateness_ms"] = round(late_ms_max, 3)
    meta["end_wall_ns"] = time.time_ns()
    with open(args.meta, "w") as f:
        json.dump(meta, f)
    time.sleep(0.5)
    sock.close()
    print(f"feeder: sent {sent} frames, max lateness {late_ms_max:.2f} ms")


if __name__ == "__main__":
    main()
