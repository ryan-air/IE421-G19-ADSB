"""Checks the generated Python feed module against the golden packet.

Run from the repository root:  python3 -m unittest discover -s protocol/tests
"""

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "protocol" / "generated"))

import feed_messages as fm  # noqa: E402


class GoldenPacket(unittest.TestCase):
    def setUp(self):
        self.raw = (ROOT / "protocol" / "golden" / "packet_v1.bin").read_bytes()
        self.expected = json.loads((ROOT / "protocol" / "golden" / "packet_v1.json").read_text())

    def test_decode_matches_json(self):
        header, msgs = fm.decode_packet(self.raw)
        self.assertEqual(header["session"], self.expected["header"]["session"])
        self.assertEqual(header["seq"], int(self.expected["header"]["seq"]))
        self.assertEqual(len(msgs), len(self.expected["messages"]))
        for i, ((seq, _type, name, msg), exp) in enumerate(zip(msgs, self.expected["messages"])):
            self.assertEqual(seq, header["seq"] + i)
            self.assertEqual(name, exp["name"])
            # 64-bit fields are stored as strings in the JSON.
            want = {k: int(v) if isinstance(v, str) and isinstance(msg[k], int) else v
                    for k, v in exp["fields"].items()}
            self.assertEqual(msg, want)

    def test_reencode_is_identical(self):
        header, msgs = fm.decode_packet(self.raw)
        body = b""
        for _seq, _type, name, msg in msgs:
            snake = "".join("_" + c.lower() if c.isupper() else c for c in name).lstrip("_")
            enc = getattr(fm, "encode_" + snake)(msg)
            body += len(enc).to_bytes(2, "little") + enc
        self.assertEqual(fm.encode_packet_header(header) + body, self.raw)

    def test_truncation_raises(self):
        # Every truncation must raise rather than return partial data.
        for n in range(len(self.raw)):
            with self.assertRaises(ValueError, msg=f"length {n}"):
                fm.decode_packet(self.raw[:n])

    def test_rejects_what_cpp_rejects(self):
        # The three implementations must agree on which datagrams are valid.
        with self.assertRaises(ValueError):
            fm.decode_packet(self.raw + b"XYZ")  # trailing bytes
        header = bytearray(self.raw[:20])
        for count in (fm.kHeartbeatCount, fm.kEndOfSessionCount):
            header[18:20] = count.to_bytes(2, "little")
            fm.decode_packet(bytes(header))  # bare heartbeat / end-of-session: fine
            with self.assertRaises(ValueError):
                fm.decode_packet(bytes(header) + b"junk")

    def test_nulls_and_padding(self):
        self.assertEqual(fm.TRACK_UPDATE_NULLS["lat_e7"], -(2**31))
        msg = fm.decode_track_update(self.raw, 22)
        msg["callsign"] = "AB"
        self.assertEqual(fm.decode_track_update(fm.encode_track_update(msg))["callsign"], "AB      ")

    def test_sizes(self):
        self.assertEqual(fm.TRACK_UPDATE_SIZE, 72)
        self.assertEqual(fm.PACKET_HEADER_SIZE, 20)
        self.assertEqual(fm.UpdateMask.kNewTrack, 1 << 15)

    def test_numpy_dtype(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy not installed")
        dt = fm.numpy_dtype("TrackUpdate")
        self.assertEqual(dt.itemsize, 72)
        rec = np.frombuffer(self.raw, dtype=dt, count=1, offset=22)[0]
        self.assertEqual(int(rec["icao"]), 0x4840D6)
        self.assertEqual(rec["callsign"], b"KLM1023 ")


if __name__ == "__main__":
    unittest.main()
