# ADS-B Track Feed — Wire Specification v0 (draft)

**Status:** v0 draft for team review. Changes are expected before v1. Once a field is in v1 it can only be appended to, never moved.

**Source of truth:** [`schema/feed_v0.json`](schema/feed_v0.json). Field tables in this document are copied from the schema. If the two disagree, the schema wins. Generated code:

| Language | File | Use |
|---|---|---|
| C++20 | [`core/include/adsb/protocol/feed_messages.hpp`](../core/include/adsb/protocol/feed_messages.hpp) | Publisher, C++ subscribers |
| JavaScript (ES module) | [`generated/feed_messages.mjs`](generated/feed_messages.mjs) | WebSocket gateway, browser UI |
| Python | [`generated/feed_messages.py`](generated/feed_messages.py) | Camera fusion, analysis, notebooks (optional NumPy dtypes) |

Regenerate after editing the schema with `python3 protocol/gen.py`. CI runs `python3 protocol/gen.py --check`, plus golden-packet tests in all three languages, so the implementations cannot drift apart.

---

## 1. Design choices

| Choice | Reason |
|---|---|
| **MoldUDP64 framing** (Nasdaq) | A real exchange transport: sequenced, with gap recovery by retransmit request, and the same framing the ITCH workstream (README_UPDATES.md §14) replays. |
| **Little-endian integers** | Every host we use is little-endian, so encode and decode are plain copies. This deliberately differs from MoldUDP64 (big-endian). ITCH payloads are carried unchanged in their own big-endian format. |
| **Fixed-size messages, natural alignment** | No parsing branches. `TrackUpdate` is exactly one 64-byte cache line, and every field sits at an offset that is a multiple of its size. The generator rejects schemas that would need padding. |
| **Full state per update + change mask** | A subscriber can render a track from any single `TrackUpdate`. `update_mask` says what changed, so consumers that care about one field skip the rest. |
| **Explicit null values** | Unknown fields hold a sentinel (e.g. `INT32_MIN`) rather than 0, which is a valid value for most fields. Generated as `TrackUpdate::kLatE7Null` (C++), `TrackUpdateNull` (JS), `TRACK_UPDATE_NULLS` (Python). |
| **Event time** | `event_ts_ns` is the receive time of the frame that caused the update, never the time of publication. Replays are therefore deterministic (README_UPDATES.md §11.2). |

---

## 2. Packet framing

Every UDP datagram:

```
+-------------------------------+
| PacketHeader (20 bytes)       |
+-------------------------------+
| u16 length | message (length) |   repeated `count` times
+-------------------------------+
```

**`PacketHeader`** (packed, 20 bytes)

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `session` | char[10] | Publisher session id. Changes on every publisher restart; a subscriber that sees a new session discards its state and takes a snapshot. |
| 10 | `seq` | u64 | Sequence number of the **first** message in the packet. Message *i* has sequence `seq + i`. For a heartbeat, the next sequence number the publisher will use. |
| 18 | `count` | u16 | Number of messages. `0` = heartbeat. `65535` = end of session. |

Rules:
- Sequence numbers start at 1 for each session, count **messages** (not packets), and never repeat or skip within a session.
- A datagram is at most **1,472 bytes** (1,500-byte MTU − IP − UDP), which holds 22 `TrackUpdate`s.
- Heartbeat or end-of-session packets have no body.
- Receivers must reject a datagram whose blocks run past its end or leave trailing bytes, and a heartbeat or end-of-session packet that has a body. `parse_packet()` in C++, `decodePacket` in JS, and `decode_packet` in Python all enforce this, and the golden-packet tests check that they agree.
- Receivers must skip (not fail on) messages with an unknown `msg_type`, so new message types can be added.

**`RetransmitRequest`** (packed, 20 bytes, unicast UDP to the retransmit server): `session` char[10] @0, `seq` u64 @10, `count` u16 @18. The server replies with ordinary data packets for that range, if it still holds them.

---

## 3. Messages

The first byte of every message is its ASCII type, and the second is the schema version (`kVersion = 0`).

### `TrackUpdate` — `'U'`, 64 bytes

| Offset | Field | Type | Unit / meaning | Null |
|---|---|---|---|---|
| 0 | `msg_type` | u8 | `'U'` | |
| 1 | `version` | u8 | 0 | |
| 2 | `update_mask` | u16 | `UpdateMask` bits: fields that changed | |
| 4 | `icao` | u32 | 24-bit address (low bits) | |
| 8 | `event_ts_ns` | u64 | Receive time of the causing frame, ns | |
| 16 | `radio_ts` | u64 | Beast 12 MHz counter or own-demodulator sample index; **top 4 bits = receiver line id** | |
| 24 | `lat_e7` | i32 | Latitude, 1e-7° | `INT32_MIN` |
| 28 | `lon_e7` | i32 | Longitude, 1e-7° | `INT32_MIN` |
| 32 | `alt_ft` | i32 | Altitude, ft (`flags.kAltitudeGnss` says which kind) | `INT32_MIN` |
| 36 | `gs_dkt` | u16 | Ground speed, 0.1 kt | `0xFFFF` |
| 38 | `track_cdeg` | u16 | True track, 0.01°, [0, 36000) | `0xFFFF` |
| 40 | `vrate_fpm` | i16 | Vertical rate, ft/min, clamped to ±32767 | `INT16_MIN` |
| 42 | `squawk` | u16 | Mode A code, octal digits as hex nibbles (7700 → `0x7700`) | `0xFFFF` |
| 44 | `callsign` | char[8] | Space-padded; all spaces if unknown | |
| 52 | `category` | u8 | Emitter category, e.g. `0xA3`; 0 if unknown | |
| 53 | `nic` | u8 | Navigation integrity category; 0 if unknown (not yet populated) | |
| 54 | `flags` | u8 | `TrackFlags` bits | |
| 55 | `reserved` | u8 | 0 | |
| 56 | `publish_ts_ns` | u64 | Publisher software timestamp just before send; 0 inside the pipeline | |

**`UpdateMask` bits:** 0 `kPosition`, 1 `kAltitude`, 2 `kGroundSpeed`, 3 `kTrack`, 4 `kVerticalRate`, 5 `kCallsign`, 6 `kSquawk`, 7 `kCategory`, 8 `kAirGround`, 15 `kNewTrack`.

**`TrackFlags` bits:** 0 `kAltitudeGnss`, 1 `kOnGround`, 2 `kNonIcaoAddress`, 3 `kSurfacePosition`.

### `TrackDelete` — `'D'`, 16 bytes

`msg_type` u8 @0, `version` u8 @1, `reason` u8 @2 (1 = expired, 2 = evicted), `reserved` u8 @3, `icao` u32 @4, `event_ts_ns` u64 @8.

### `SnapshotBegin` — `'S'`, 24 bytes / `SnapshotEnd` — `'E'`, 16 bytes

These are sent by the snapshot server over TCP. The same framing is used, with each message prefixed by its u16 length. A snapshot is:

```
SnapshotBegin { track_count, as_of_seq, event_ts_ns }
TrackUpdate x track_count
SnapshotEnd   { as_of_seq }
```

The snapshot reflects every incremental message with `seq <= as_of_seq`. The subscriber then applies buffered incremental messages with `seq > as_of_seq` and discards older ones.

---

## 4. Subscriber behaviour

The decision logic is implemented in `SequenceTracker` (`core/include/adsb/protocol/moldudp64.hpp`). The full protocol, including loss, duplication, and reordering, is model-checked in TLA+ in [`spec/`](../spec/).

| Packet vs. `expected` | Action |
|---|---|
| `seq == expected` | Apply all messages; `expected += count` |
| `seq + count <= expected` | Duplicate: drop |
| `seq < expected < seq + count` | Overlap: skip the first `expected − seq` messages, apply the rest |
| `seq > expected` | Gap: buffer this packet. Send a `RetransmitRequest` for `[expected, seq)`; if the server no longer holds that range, take a snapshot |
| Heartbeat with `seq > expected` | Gap at the tail: request `[expected, seq)` |
| New `session` | Discard state, take a snapshot, then reset the tracker to the new session |
| Packet from the previous session (after the switch) | Stale (UDP reordering): drop |
| End of session | Stop; expect a new session |

---

## 5. Open questions for review

1. **`nic` / position quality:** populate from TC 31 (operational status) once it is decoded, or replace with NUCp derived from the position type code?
2. **Heading and airspeed:** these are decoded (velocity subtypes 3/4) but not carried. Add them in v1, at the cost of a second cache line, or as a separate `'V'` message?
3. **Conflated feed** (README_UPDATES.md §10.2): same messages on a separate multicast group, or a dedicated compact message?
4. **Multicast group and port:** placeholder `239.19.0.1:41900` for the incremental feed, `239.19.0.2` for fusion. Confirm these against the lab network.
