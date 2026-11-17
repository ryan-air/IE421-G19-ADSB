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

**`RetransmitRequest`** (packed, 20 bytes, unicast UDP to the retransmit server): `session` char[10] @0, `seq` u64 @10, `count` u16 @18. The server replies to the requester's address:

| Server state | Reply |
|---|---|
| Holds `seq` | Ordinary data packets of the same session covering `[seq, seq + count)`, truncated to what has been published and to the server's per-request cap (4,096 messages by default). The subscriber asks again for anything still missing. |
| `seq` has left its ring | One header-only packet (`count` 0) whose `seq` is the **oldest sequence number the server still holds**. This is the NAK: on the request socket it means "that range is gone", and the subscriber takes a snapshot. (On the multicast feed the same shape is a heartbeat; the socket it arrives on tells them apart.) |
| `seq` not published yet, wrong session, or malformed request | Nothing. The subscriber retries after its timeout. |

**End of session.** The publisher sends the end-of-session packet three times when it stops, then repeats it at the heartbeat interval for as long as it keeps running (its recovery servers stay up for a configurable linger time). The marker cannot be retransmitted, so a subscriber that lost every copy would otherwise never learn that the session ended.

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

These are sent by the snapshot server over TCP. The subscriber connects, and the server writes one snapshot and closes the connection:

```
session        char[10]       the publisher session the snapshot belongs to
u16 len + SnapshotBegin { track_count, as_of_seq, event_ts_ns }
u16 len + TrackUpdate   x track_count, in strictly increasing ICAO order
u16 len + SnapshotEnd   { as_of_seq }
```

The snapshot reflects every incremental message with `seq <= as_of_seq`. The subscriber then applies buffered incremental messages with `seq > as_of_seq` and discards older ones. If the subscriber already holds a later state (`expected > as_of_seq + 1`), it keeps its own state. A receiver rejects the stream if a length or type is wrong, the count does not match, the ICAO order is broken, the two `as_of_seq` values differ, or any byte follows `SnapshotEnd`. The session prefix lets a subscriber notice that the publisher restarted between its request and the answer.

---

## 4. Subscriber behaviour

The full subscriber is `SubscriberCore` (`core/include/adsb/feed/subscriber.hpp`), with `SequenceTracker` (`core/include/adsb/protocol/moldudp64.hpp`) as a lighter classifier for simple consumers. Both treat a packet with `seq` 0, or one whose messages would run past 2^64 − 1, as malformed: sequence numbers never wrap. The protocol, including loss, duplication, and reordering, is model-checked in TLA+ in [`spec/`](../spec/). Sessions are not in the model; they are covered by unit tests and a randomized simulation.

A subscriber buffers what arrives ahead of `expected` (16,384 messages by default) and requests `[expected, first buffered)` in chunks of up to 1,024 messages. It re-requests after 10 ms without progress.

| Packet vs. `expected` | Action |
|---|---|
| `seq == expected` | Apply all messages; `expected += count` |
| `seq + count <= expected` | Duplicate: drop |
| `seq < expected < seq + count` | Overlap: skip the first `expected − seq` messages, apply the rest |
| `seq > expected` | Gap: buffer this packet. Send a `RetransmitRequest` for `[expected, seq)`; if the server no longer holds that range, take a snapshot |
| Heartbeat with `seq > expected` | Gap at the tail: request `[expected, seq)` |
| New `session` | Discard state. If the packet starts at `seq` 1, nothing else is needed (the state as of 0 is empty); otherwise take a snapshot |
| Packet from the previous session (after the switch) | Stale (UDP reordering): drop |
| End of session | Like a heartbeat: recover any gap before it. The session is complete once every message before its `seq` is applied |
| Retransmit reply (data) | Same as a multicast data packet |
| NAK whose `seq > expected` | The gap is older than the server's ring: take a snapshot. A NAK with `seq <= expected` is stale and ignored |

Browsers do not join the multicast group. The WebSocket gateway (`gateway/`) is a subscriber like any other, with a JavaScript port of `SubscriberCore`. It forwards the same `TrackUpdate` and `TrackDelete` bytes to browsers in conflated frames, with per-client flow control. That format is specified in [`gateway/README.md`](../gateway/README.md#protocol-gateway--browser).

---

## 5. Open questions for review

1. **`nic` / position quality:** populate from TC 31 (operational status) once it is decoded, or replace with NUCp derived from the position type code?
2. **Heading and airspeed:** these are decoded (velocity subtypes 3/4) but not carried. Add them in v1, at the cost of a second cache line, or as a separate `'V'` message?
3. **Conflated feed** (README_UPDATES.md §10.2): same messages on a separate multicast group, or a dedicated compact message?
4. **Multicast group and ports:** defaults are `239.19.0.1:41900` for the incremental feed, UDP 41901 for retransmit requests, TCP 41902 for snapshots, and `239.19.0.2` for fusion. Confirm these against the lab network.
