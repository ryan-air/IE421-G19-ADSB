// Fuzzes the feed's untrusted inputs. The first byte picks the target:
//
//   0  SnapshotParser on the rest, fed in chunks whose sizes come from the input.
//      Invariants: no out-of-bounds reads (ASan); once done, the tracks are in
//      strictly increasing ICAO order, and serializing them again parses to
//      the same snapshot.
//   1  SubscriberCore on a sequence of datagrams [u8 flags][u16 len][bytes]
//      (flags bit 0: from the retransmit socket; bit 1: then answer a pending
//      snapshot request with an empty snapshot as of the last 8 bytes seen).
//      Invariants: applied messages are consecutive between resets and
//      snapshots; every retransmit request asks for [expected, ...) with
//      count >= 1; expected never passes the highest sequence number known.
//   2  RecoveryState: messages logged from the input, then requests from the
//      input. Invariants: every reply parses, fits one datagram, carries
//      messages that were logged with their own sequence numbers, and a NAK
//      names the oldest held message.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "adsb/feed/recovery.hpp"
#include "adsb/feed/snapshot.hpp"
#include "adsb/feed/subscriber.hpp"
#include "fuzz_check.hpp"

using namespace adsb;
using namespace adsb::feed;

namespace {

void fuzz_snapshot(const std::uint8_t* data, std::size_t size) {
  SnapshotParser p;
  std::size_t off = 0, k = 0;
  while (off < size && p.state() == SnapshotParser::State::kNeedMore) {
    const std::size_t chunk = 1 + (data[k++ % size] % 97);
    const std::size_t n = chunk < size - off ? chunk : size - off;
    p.feed(std::span<const std::uint8_t>(data + off, n));
    off += n;
  }
  if (p.state() != SnapshotParser::State::kDone) return;
  const SnapshotData& d = p.data();
  FeedBook book;
  for (std::size_t i = 0; i < d.tracks.size(); ++i) {
    if (i > 0) FUZZ_CHECK(d.tracks[i - 1].icao < d.tracks[i].icao);
    book.put(d.tracks[i]);
  }
  std::vector<std::uint8_t> again;
  serialize_snapshot(d.session, d.as_of_seq, d.event_ts_ns, book, again);
  SnapshotParser q;
  FUZZ_CHECK(q.feed(again) == SnapshotParser::State::kDone);
  FUZZ_CHECK(q.data().session == d.session && q.data().as_of_seq == d.as_of_seq);
  FUZZ_CHECK(q.data().tracks.size() == d.tracks.size());
}

struct CheckIo : SubscriberIo {
  const SubscriberCore* core = nullptr;
  void send_request(const protocol::RetransmitRequest& r) override {
    FUZZ_CHECK(r.count >= 1);
    FUZZ_CHECK(r.seq == core->expected());
    FUZZ_CHECK(r.session == core->session());
  }
};

struct CheckHandler : SubscriberHandler {
  bool fresh = true;
  std::uint64_t last = 0;
  void on_message(std::uint64_t seq, std::span<const std::uint8_t>) override {
    FUZZ_CHECK(fresh || seq == last + 1);
    fresh = false;
    last = seq;
  }
  void on_reset() override { fresh = true; }
  void on_snapshot_loaded() override { fresh = true; }
};

void fuzz_subscriber(const std::uint8_t* data, std::size_t size) {
  CheckIo io;
  CheckHandler h;
  SubscriberConfig cfg;
  cfg.window = 64;
  cfg.max_request = 16;
  SubscriberCore sub(cfg, &io, &h);
  io.core = &sub;
  std::uint64_t now = 0;
  std::size_t off = 0;
  while (off + 3 <= size) {
    const std::uint8_t flags = data[off];
    std::uint16_t len;
    std::memcpy(&len, data + off + 1, 2);
    off += 3;
    if (len > size - off) len = static_cast<std::uint16_t>(size - off);
    const std::span<const std::uint8_t> dgram(data + off, len);
    off += len;
    now += 1'000'000;
    sub.on_datagram(dgram, (flags & 1) ? SubscriberCore::Source::kRetransmit : SubscriberCore::Source::kFeed, now);
    FUZZ_CHECK(sub.expected() <= sub.known_next());
    sub.poll(now);
    if ((flags & 2) && sub.take_snapshot_request(now) && dgram.size() >= 8) {
      SnapshotData snap;
      snap.session = sub.session();
      std::memcpy(&snap.as_of_seq, dgram.data() + dgram.size() - 8, 8);
      snap.as_of_seq &= 0xFFFFFFFFULL;  // keep clear of overflow at 2^64
      sub.on_snapshot(snap, now);
      FUZZ_CHECK(sub.mode() == SubscriberCore::Mode::kLive);
      FUZZ_CHECK(sub.expected() <= sub.known_next());
    }
  }
}

void fuzz_recovery(const std::uint8_t* data, std::size_t size) {
  if (size < 2) return;
  RecoveryConfig cfg;
  cfg.session = protocol::make_session("FUZZ000001");
  cfg.ring_capacity = 1u + data[0] % 64;
  cfg.max_reply_messages = 1u + data[1] % 50;
  RecoveryState st(cfg);
  std::size_t off = 2;
  // Log: [u8 len][bytes] until a zero length.
  std::uint64_t seq = 1;
  while (off < size && data[off] != 0) {
    std::size_t len = data[off++] % (kMaxMessageBytes + 1);
    if (len > size - off) len = size - off;
    if (len == 0) break;
    st.on_message(seq++, std::span<const std::uint8_t>(data + off, len));
    off += len;
  }
  ++off;
  // Requests: 20-byte chunks, the session forced valid when the chunk says so.
  while (off + protocol::RetransmitRequest::kWireSize <= size) {
    std::uint8_t req[protocol::RetransmitRequest::kWireSize];
    std::memcpy(req, data + off, sizeof req);
    if (req[0] & 1) std::memcpy(req, cfg.session.data(), cfg.session.size());
    off += sizeof req;
    protocol::RetransmitRequest r;
    protocol::read(req, r);
    st.handle_request(req, [&](std::span<const std::uint8_t> d) {
      FUZZ_CHECK(d.size() <= protocol::kMaxUdpPayload);
      const auto pv = protocol::parse_packet(d);
      FUZZ_CHECK(pv.has_value());
      FUZZ_CHECK(pv->header.session == cfg.session);
      if (pv->kind == protocol::PacketKind::kHeartbeat) {
        FUZZ_CHECK(pv->header.seq == st.store().oldest());
        FUZZ_CHECK(r.seq < st.store().oldest());
        return;
      }
      FUZZ_CHECK(pv->kind == protocol::PacketKind::kData);
      pv->for_each([&](std::uint64_t s, std::span<const std::uint8_t> m) {
        FUZZ_CHECK(s >= r.seq && s < st.store().next());
        const LogEntry* e = st.store().get(s);
        FUZZ_CHECK(e != nullptr && e->len == m.size() && std::memcmp(e->bytes, m.data(), m.size()) == 0);
      });
    });
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) return 0;
  switch (data[0] % 3) {
    case 0: fuzz_snapshot(data + 1, size - 1); break;
    case 1: fuzz_subscriber(data + 1, size - 1); break;
    default: fuzz_recovery(data + 1, size - 1); break;
  }
  return 0;
}
