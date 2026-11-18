#!/usr/bin/env python3
"""Observation probes for the Fall 2025 pipeline. Each mode runs until killed.

  db   polls the aircraft_real table; logs every change of (hex -> lat, lon)
  api  polls GET /api/aircraft (what the Fall 2025 map polls); logs changes,
       and also logs a full snapshot periodically for staleness analysis
  sbs  connects to readsb's SBS-1 port; logs every message line with its arrival time
  cpu  samples CPU time of named processes from /proc once per second
  ws   the new pipeline's map feed: connects to the gateway's WebSocket like a
       browser does, logs every position change with the arrival time of its
       frame, and a full snapshot of the table periodically

All output is CSV with a wall-clock nanosecond timestamp first. None of the
Fall 2025 code is modified; the probes only read what a user would see.
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request


def now_ns():
    return time.time_ns()


def run_db(args, out):
    import psycopg2

    conn = psycopg2.connect(os.environ["DATABASE_URL"], sslmode="require")
    conn.autocommit = True
    cur = conn.cursor()
    last = {}
    out.write("t_ns,hex,lat,lon\n")
    while True:
        t = now_ns()
        cur.execute("SELECT hex, lat, lon FROM aircraft_real WHERE lat IS NOT NULL AND lon IS NOT NULL")
        for hx, lat, lon in cur.fetchall():
            key = (round(lat, 6), round(lon, 6))
            if last.get(hx) != key:
                last[hx] = key
                out.write(f"{t},{hx},{lat:.7f},{lon:.7f}\n")
        out.flush()
        time.sleep(max(0.0, args.interval - (now_ns() - t) / 1e9))


def run_api(args, out, snap):
    url = f"http://127.0.0.1:{args.api_port}/api/aircraft?window=120"
    last = {}
    out.write("t_ns,hex,lat,lon\n")
    snap.write("t_ns,hex,lat,lon\n")
    next_snapshot = 0
    while True:
        t = now_ns()
        try:
            with urllib.request.urlopen(url, timeout=10) as r:
                body = json.load(r)
        except Exception as e:  # server not up yet, or overloaded
            print(f"api probe: {e}", file=sys.stderr)
            time.sleep(0.2)
            continue
        rows = body.get("aircraft", [])
        take_snapshot = t >= next_snapshot
        for a in rows:
            hx, lat, lon = a.get("hex"), a.get("lat"), a.get("lon")
            if hx is None or lat is None or lon is None:
                continue
            lat, lon = float(lat), float(lon)
            key = (round(lat, 6), round(lon, 6))
            if last.get(hx) != key:
                last[hx] = key
                out.write(f"{t},{hx},{lat:.7f},{lon:.7f}\n")
            if take_snapshot:
                snap.write(f"{t},{hx},{lat:.7f},{lon:.7f}\n")
        if take_snapshot:
            next_snapshot = t + int(args.snapshot_every * 1e9)
            snap.flush()
        out.flush()
        time.sleep(max(0.0, args.interval - (now_ns() - t) / 1e9))


def run_sbs(args, out):
    for _ in range(200):
        try:
            s = socket.create_connection(("127.0.0.1", args.sbs_port), timeout=5)
            break
        except OSError:
            time.sleep(0.05)
    else:
        sys.exit("sbs probe: cannot connect")
    s.settimeout(None)
    buf = b""
    out.write("t_ns,line\n")
    while True:
        chunk = s.recv(65536)
        if not chunk:
            return
        t = now_ns()
        buf += chunk
        *lines, buf = buf.split(b"\n")
        for ln in lines:
            ln = ln.strip()
            if ln:
                out.write(f"{t},{ln.decode('ascii', 'replace')}\n")
        out.flush()


def run_ws(args, out, snap):
    """Minimal WebSocket client for the gateway (gateway/README.md, Protocol).

    No flow control (no hello): the probe reads as fast as it can, and the
    gateway writes a frame only when the previous one is in the kernel, so a
    probe that falls behind receives merged frames, as a browser would. The
    timestamp is taken when the recv() that completes a frame returns,
    before any parsing.
    """
    for _ in range(300):
        try:
            s = socket.create_connection(("127.0.0.1", args.ws_port), timeout=5)
            break
        except OSError:
            time.sleep(0.05)
    else:
        sys.exit("ws probe: cannot connect")
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /gateway/ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            sys.exit("ws probe: closed during handshake")
        buf += chunk
    head, _, buf = buf.partition(b"\r\n\r\n")
    t = now_ns()  # frames that arrived with the handshake
    if not head.startswith(b"HTTP/1.1 101"):
        sys.exit(f"ws probe: handshake refused: {head[:80]!r}")
    s.settimeout(None)
    table = {}  # hex -> (lat, lon)
    out.write("t_ns,hex,lat,lon\n")
    snap.write("t_ns,hex,lat,lon\n")
    next_snapshot = 0
    null = -2 ** 31
    while True:
        # Parse every complete frame in the buffer.
        while True:
            if len(buf) < 2:
                break
            op, ln, off = buf[0] & 0x0F, buf[1] & 0x7F, 2
            if ln == 126:
                if len(buf) < 4:
                    break
                ln, off = struct.unpack_from(">H", buf, 2)[0], 4
            elif ln == 127:
                if len(buf) < 10:
                    break
                ln, off = struct.unpack_from(">Q", buf, 2)[0], 10
            if len(buf) < off + ln:
                break
            payload, buf = buf[off:off + ln], buf[off + ln:]
            if op == 8:
                return
            if op != 2 or len(payload) < 16:
                continue
            kind, _ver, _flags, count = struct.unpack_from("<BBHI", payload, 0)
            if kind == 0x53:  # snapshot: replace the table
                table.clear()
            p = 16
            for _ in range(count):
                (mlen,) = struct.unpack_from("<H", payload, p)
                m = payload[p + 2:p + 2 + mlen]
                p += 2 + mlen
                if mlen >= 64 and m[0] == 0x55:
                    icao, = struct.unpack_from("<I", m, 4)
                    la, lo = struct.unpack_from("<ii", m, 24)
                    hx = f"{icao:06X}"
                    if la == null or lo == null:
                        table.pop(hx, None)
                        continue
                    pos = (la / 1e7, lo / 1e7)
                    if table.get(hx) != pos:
                        table[hx] = pos
                        out.write(f"{t},{hx},{pos[0]:.7f},{pos[1]:.7f}\n")
                elif mlen >= 16 and m[0] == 0x44:
                    icao, = struct.unpack_from("<I", m, 4)
                    table.pop(f"{icao:06X}", None)
        out.flush()  # the probe is killed with SIGTERM: keep nothing buffered
        chunk = s.recv(1 << 20)
        if not chunk:
            return
        t = now_ns()
        buf += chunk
        if t >= next_snapshot:
            for hx, (la, lo) in table.items():
                snap.write(f"{t},{hx},{la:.7f},{lo:.7f}\n")
            next_snapshot = t + int(args.snapshot_every * 1e9)
            snap.flush()


def run_cpu(args, out):
    hz = os.sysconf("SC_CLK_TCK")
    out.write("t_ns,name,pid,cpu_seconds\n")
    while True:
        t = now_ns()
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                cmd = open(f"/proc/{pid}/cmdline", "rb").read().replace(b"\0", b" ").decode(errors="replace")
                name = next((n for n in args.names if n in cmd), None)
                if not name:
                    continue
                fields = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
                cpu = (int(fields[11]) + int(fields[12])) / hz  # utime + stime
                out.write(f"{t},{name},{pid},{cpu:.3f}\n")
            except (OSError, IndexError, ValueError):
                continue
        out.flush()
        time.sleep(1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["db", "api", "sbs", "cpu", "ws"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--snapshot-out")
    ap.add_argument("--interval", type=float, default=0.1, help="poll period in seconds (db, api)")
    ap.add_argument("--snapshot-every", type=float, default=1.0)
    ap.add_argument("--api-port", type=int, default=3000)
    ap.add_argument("--sbs-port", type=int, default=30003)
    ap.add_argument("--ws-port", type=int, default=8080)
    ap.add_argument("--names", nargs="*", default=[])
    args = ap.parse_args()
    with open(args.out, "w") as out:
        if args.mode == "db":
            run_db(args, out)
        elif args.mode == "api":
            with open(args.snapshot_out, "w") as snap:
                run_api(args, out, snap)
        elif args.mode == "sbs":
            run_sbs(args, out)
        elif args.mode == "ws":
            with open(args.snapshot_out, "w") as snap:
                run_ws(args, out, snap)
        else:
            run_cpu(args, out)


if __name__ == "__main__":
    main()
