# ADS-B Track Feed — Wire Specification v1

**Status:** version 1 (`protocol/schema/feed_v1.json`), used by every component in this repository. Fields are only appended to or assigned from reserved bytes, never moved.

**Source of truth:** [`schema/feed_v1.json`](schema/feed_v1.json). Field tables in this document are copied from the schema. If the two disagree, the schema wins. Generated code:

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
| **Fixed-size messages, natural alignment** | No parsing branches. `TrackUpdate` is 72 bytes (one 64-byte cache line until version 1 added heading and airspeed, §5), and every field sits at an offset that is a multiple of its size. The generator rejects schemas that would need padding. |
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

The first byte of every message is its ASCII type, and the second is the schema version (`kVersion = 1`). Version 1 (2026-11-23) grew `TrackUpdate` from 64 to 72 bytes for heading, airspeed and the populated `nic` / `adsb_version` (§5); every component in this repository speaks version 1 only, and the generated code has no version 0 reader.

### `TrackUpdate` — `'U'`, 72 bytes

| Offset | Field | Type | Unit / meaning | Null |
|---|---|---|---|---|
| 0 | `msg_type` | u8 | `'U'` | |
| 1 | `version` | u8 | 1 | |
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
| 53 | `nic` | u8 | Navigation integrity category of the current position, 0–11 (below); 0 = unknown or no position | |
| 54 | `flags` | u8 | `TrackFlags` bits | |
| 55 | `adsb_version` | u8 | ADS-B version from operational status (TC 31): 0 DO-260, 1 DO-260A, 2 DO-260B | `0xFF` (no TC 31 yet) |
| 56 | `publish_ts_ns` | u64 | Publisher software timestamp just before send; 0 inside the pipeline | |
| 64 | `heading_cdeg` | u16 | Heading, 0.01°, [0, 36000), from airborne velocity subtypes 3/4, or Comm-B BDS 6,0 (below); reference in `flags.kHeadingTrue` | `0xFFFF` |
| 66 | `airspeed_kt` | u16 | Airspeed, kt, from airborne velocity subtypes 3/4, or Comm-B BDS 6,0 (indicated); `flags.kAirspeedTrue` = TAS, else IAS | `0xFFFF` |
| 68 | `nac_p` | u8 | Navigation accuracy category for position, 0–11, from the latest TC 31 (versions 1/2) or TC 29; 0 = unknown (or EPU ≥ 10 NM) | |
| 69 | `sil` | u8 | Source integrity level: bits 0–1 the SIL (0–3), bit 2 set when per sample (clear: per hour); from TC 31 (versions 1/2) or TC 29; 0 = unknown | |
| 70 | `nuc_p` | u8 | Navigation uncertainty category for position, 0–9, of the current position of a version 0 aircraft, from its type code; 0 = unknown or a version 1/2 aircraft | |
| 71 | `reserved` | u8 | 0 | |

A datagram holds up to 19 `TrackUpdate`s (22 in version 0). The shared-memory ring's 128-byte slots carry 72-byte payloads since its layout version 2.

**`nic`.** Computed for every position message when it arrives, from its type code, the aircraft's ADS-B version and the NIC supplements: A and C from the latest operational status (TC 31 from DF17 or DF18 CF 0/1 only; version 1 or 2, since versions 3–7 are reserved and only reported; C only from a surface status with capability class code 0), each counting for 60 s after the status that set it; B from bit 8 of the airborne position message (DF17 or DF18 CF 0/1; for TIS-B and ADS-R that bit is the IMF flag). A global CPR fix carries the lower NIC of its two frames, a local fix against the track's previous position the lower of its frame's and the track's, a local fix against the receiver its frame's. Until a TC 31 arrives the version 0 rules apply (the NIC that the type code alone implies). `flags.kHeadingTrue` is set only while a heading is known. The rules are readsb's (`track.c`, `compute_nic`, v3.16.21), i.e. DO-260B Tables 2-14 and 2-15 with readsb's readings of versions 0 and 1; `modes::position_nic()` in `core/include/adsb/modes/decoder.hpp` implements them. A later TC 31 changes `adsb_version` at once but `nic` only with the next positions.

**Accuracy (bytes 68–70, 2026-11-24).** These were the `reserved` word, which was always 0. 0 means unknown, which is also each scale's worst value, so a reader that ignored the bytes reads them correctly, and the version stays 1.
- **`nac_p` and `sil`:** from the newest operational status (TC 31, versions 1/2) or target state (TC 29). Both come from the same sources as `adsb_version`: DF17, or DF18 CF 0/1. Version 1 has no SIL supplement, and its `sil` is shown as per hour. A status for version 0 or a reserved version (3–7) clears both. Neither expires otherwise: a track keeps them until it ends.
- **`nuc_p`:** computed per position message like `nic`, for aircraft whose version is 0, reserved (3–7, read as 0, as for `nic`) or not yet known. A global fix takes the lower value of its pair. Surface type codes 5–8 give 9, 8, 7, 6; airborne 9–18 give 9…0; GNSS 20–22 give 9, 8, 0 (`modes::position_nuc()`).
- **Comm-B air data:** an aircraft that sends no airborne velocity subtype 3/4 (many Mode S-only aircraft) gets `heading_cdeg` and `airspeed_kt` from Comm-B BDS 6,0 replies: magnetic heading and indicated airspeed, with `flags.kAirDataCommB` set. When ADS-B air data returns with only one of the two fields, the other (still Comm-B's) is dropped (null) rather than relabelled. This applies only when there has been no subtype 3/4 for 30 s, and only when the previous BDS 6,0 reply, at most 30 s earlier, agrees: heading within 10°, IAS within 20 kt. The register is inferred from the reply's contents, so one reply alone could be another register's.

**`UpdateMask` bits:** 0 `kPosition`, 1 `kAltitude`, 2 `kGroundSpeed`, 3 `kTrack`, 4 `kVerticalRate`, 5 `kCallsign`, 6 `kSquawk`, 7 `kCategory`, 8 `kAirGround`, 9 `kHeading` (heading or its reference), 10 `kAirspeed` (airspeed or TAS/IAS), 11 `kIntegrity` (`nic`, `adsb_version`, `nac_p`, `sil` or `nuc_p`), 15 `kNewTrack`.

**`TrackFlags` bits:** 0 `kAltitudeGnss`, 1 `kOnGround`, 2 `kNonIcaoAddress`, 3 `kSurfacePosition`, 4 `kHeadingTrue` (heading is relative to true north: TC 31 HRD = 0, version 1 or later; otherwise magnetic), 5 `kAirspeedTrue` (true airspeed; otherwise indicated), 6 `kAirDataCommB` (heading and airspeed came from Comm-B BDS 6,0, not ADS-B).

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
| New `session` | Discard state. If the packet starts at `seq` 1, nothing else is needed for the data (the state as of 0 is empty); otherwise take a snapshot. A *change* of session at `seq` 1 is also confirmed with the snapshot server in the background, staying live meanwhile: around a restart a late packet of the old session can arrive after the new session's first packets, and session ids are opaque, so without the check a subscriber could switch to the dead session and drop the live one as stale. The snapshot server is the live publisher: if its snapshot is of another session, the subscriber switches to that one (found by deterministic simulation, `core/include/adsb/feed/dst.hpp`) |
| Snapshot answer | Use it only if no change of session happened since it was requested; otherwise it is stale whatever session it carries (it may come from a server that has died since, and after a switch away and back the session can even match). A snapshot of another session that passes this check is adopted with its session: the server is the live publisher |
| Packet from the previous session (after the switch) | Stale (UDP reordering): drop |
| End of session | Like a heartbeat: recover any gap before it. The session is complete once every message before its `seq` is applied |
| Retransmit reply (data) | Same as a multicast data packet |
| NAK whose `seq > expected` | The gap is older than the server's ring: take a snapshot. A NAK with `seq <= expected` is stale and ignored |

**Same host: the shared-memory ring.** A subscriber on the publisher's machine can read the feed from a POSIX shared-memory object (`adsb-live --shm /adsb-feed`, `adsb-publish --shm ...`) instead of multicast. It carries the same messages under the same sequence numbers (§3), one per 128-byte slot. There are no packets, heartbeats or retransmissions: the ring is its own retransmit buffer, and a reader that falls a full lap behind (the slot holds a later sequence number) takes a snapshot from the TCP snapshot server and continues after its `as_of_seq`. The layout and slot protocol are documented in `core/include/adsb/feed/shm_ring.hpp`; `ShmSubscriber` (`shm_subscriber.hpp`) implements join, lap recovery, restarts (a new publisher replaces the object under the same name) and end of session. A publisher creates its ring before its snapshot server answers, and a subscriber checks after each snapshot fetch that the ring behind the name is still the one it reads, so a snapshot is never paired with a previous run's ring (even if a restart reused the session id, which §2 forbids).

**The conflated feed** (README_UPDATES.md §10.2) carries only the latest state per aircraft, at a fixed rate (default 10 Hz), for slow consumers such as the gateway and camera fusion. It is an ordinary feed in every other respect:
- **Framing and recovery:** the same messages and framing, on its own multicast group, with its own session id and sequence numbers, and its own retransmit and snapshot servers. Subscribers, including the gateway, use it unchanged.
- **Ticks:** on each tick, every aircraft that changed since the last tick gets one `TrackUpdate` with its latest state. Its `update_mask` is the union of everything that changed since the aircraft was last sent, and `publish_ts_ns` is the conflated publisher's own time. A deleted aircraft gets a `TrackDelete`, unless it was created and deleted within one tick, in which case it is never sent.
- **Guarantee:** right after every tick, the conflated book holds exactly the main feed's aircraft with the same state. Only `update_mask` and `publish_ts_ns` differ, which `FeedBook::state_digest()` ignores.
- **Rate cap:** an aircraft is sent at most once per tick. On ADS-B traffic, where aircraft average about 2 updates/s, the conflated feed sends 96% of the main feed's updates at 10 Hz and about 49% at 1 Hz. At 10 Hz its value is the per-aircraft cap, not bandwidth.

Publish it with `--conflate GROUP:PORT` (and `--conflate-hz N`) on `adsb-live` or `adsb-publish`.

Browsers do not join the multicast group. The WebSocket gateway (`gateway/`) is a subscriber like any other, with a JavaScript port of `SubscriberCore`. It forwards the same `TrackUpdate` and `TrackDelete` bytes to browsers in conflated frames, with per-client flow control. That format is specified in [`gateway/README.md`](../gateway/README.md#protocol-gateway--browser).

---

## 5. Open questions for review

1. ~~**`nic` / position quality:** populate from TC 31 (operational status) once it is decoded, or replace with NUCp derived from the position type code?~~ **Decided (v1):** `nic` is populated, with readsb's rules (§3), and `adsb_version` tells consumers which DO-260 revision it follows. Version 0 aircraft get the NIC their type code implies, which is the NUCp ordering on the NIC scale; NACp, SIL and NUCp have been carried since 2026-11-24, in what was the `reserved` word.
2. ~~**Heading and airspeed:** these are decoded (velocity subtypes 3/4) but not carried. Add them in v1, at the cost of a second cache line, or as a separate `'V'` message?~~ **Decided (v1):** in `TrackUpdate`, which grows to 72 bytes. A separate message would break the rule that one `TrackUpdate` is an aircraft's whole state, on which the conflated feed, snapshots, `FeedBook` and its digest, and the gateway's per-client conflation all rely; each of them would need a second message type kept consistent with the first. The cost: 19 instead of 22 updates per datagram, and the message spans two cache lines (the shared-memory slot was 128 bytes already).
3. ~~**Conflated feed:** same messages on a separate multicast group, or a dedicated compact message?~~ **Decided:** the same messages on a separate group (§4, "The conflated feed"), so every subscriber and the gateway work on it unchanged. A compact message can still be added later as a new type.
4. **Multicast group and ports:** defaults are `239.19.0.1:41900` for the incremental feed, UDP 41901 for retransmit requests, TCP 41902 for snapshots, `239.19.0.3:41910` (retransmit 41911, snapshot 41912) for the conflated feed, and `239.19.0.2` for fusion. Confirm these against the lab network.
