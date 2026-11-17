#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "adsb/protocol/feed_messages.hpp"
#include "adsb/protocol/moldudp64.hpp"

#ifndef ADSB_GOLDEN_DIR
#error "ADSB_GOLDEN_DIR must point at protocol/golden"
#endif

namespace {

using namespace adsb::protocol;

std::vector<std::uint8_t> read_golden() {
  std::ifstream f(std::string(ADSB_GOLDEN_DIR) + "/packet_v0.bin", std::ios::binary);
  EXPECT_TRUE(f.good()) << "missing golden packet; run python3 protocol/gen.py";
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

TEST(FeedMessages, LayoutMatchesSpec) {
  EXPECT_EQ(sizeof(TrackUpdate), 64u);
  EXPECT_EQ(TrackUpdate::kWireSize, 64u);
  EXPECT_EQ(TrackDelete::kWireSize, 16u);
  EXPECT_EQ(PacketHeader::kWireSize, 20u);
  EXPECT_EQ(RetransmitRequest::kWireSize, 20u);
  // A full packet of TrackUpdates fits exactly in one MTU-sized datagram.
  EXPECT_EQ(PacketHeader::kWireSize + 22 * (2 + TrackUpdate::kWireSize), kMaxUdpPayload);
}

TEST(FeedMessages, DefaultsAreNullValues) {
  const TrackUpdate u;
  EXPECT_EQ(u.msg_type, 'U');
  EXPECT_EQ(u.version, kVersion);
  EXPECT_EQ(u.lat_e7, TrackUpdate::kLatE7Null);
  EXPECT_EQ(u.alt_ft, TrackUpdate::kAltFtNull);
  EXPECT_EQ(u.gs_dkt, TrackUpdate::kGsDktNull);
  EXPECT_EQ(u.vrate_fpm, TrackUpdate::kVrateFpmNull);
  EXPECT_EQ(u.squawk, TrackUpdate::kSquawkNull);
  EXPECT_EQ(std::string(u.callsign.data(), 8), "        ");
}

TEST(FeedMessages, GoldenPacketDecodesAndReencodesIdentically) {
  const auto golden = read_golden();
  ASSERT_EQ(golden.size(), 104u);
  const auto p = parse_packet(golden);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(std::string(p->header.session.data(), 10), "ADSB000001");
  EXPECT_EQ(p->header.seq, 1001u);
  EXPECT_EQ(p->header.count, 2u);

  std::vector<std::uint64_t> seqs;
  TrackUpdate u;
  TrackDelete d;
  p->for_each([&](std::uint64_t seq, std::span<const std::uint8_t> m) {
    seqs.push_back(seq);
    if (m[0] == TrackUpdate::kTypeChar) {
      ASSERT_EQ(m.size(), TrackUpdate::kWireSize);
      read(m.data(), u);
    } else if (m[0] == TrackDelete::kTypeChar) {
      ASSERT_EQ(m.size(), TrackDelete::kWireSize);
      read(m.data(), d);
    }
  });
  EXPECT_EQ(seqs, (std::vector<std::uint64_t>{1001, 1002}));

  // Spot-check values from protocol/schema/feed_v0.json "example".
  EXPECT_EQ(u.icao, 0x4840D6u);
  EXPECT_EQ(u.update_mask, update_mask::kPosition | update_mask::kCallsign);
  EXPECT_EQ(u.lat_e7, 522572021);
  EXPECT_EQ(u.lon_e7, 39193725);
  EXPECT_EQ(u.alt_ft, 38000);
  EXPECT_EQ(u.vrate_fpm, -832);
  EXPECT_EQ(u.squawk, 0x7700);
  EXPECT_EQ(u.radio_ts >> 60, 1u) << "line id in the top 4 bits";
  EXPECT_EQ(std::string(u.callsign.data(), 8), "KLM1023 ");
  EXPECT_EQ(d.icao, 0x4840D6u);
  EXPECT_EQ(d.reason, 1u);

  // Rebuilding the same packet must reproduce the golden bytes exactly.
  PacketBuilder b(p->header.session, p->header.seq);
  ASSERT_TRUE(b.add(u));
  ASSERT_TRUE(b.add(d));
  const auto out = b.finish();
  ASSERT_EQ(out.size(), golden.size());
  EXPECT_EQ(std::memcmp(out.data(), golden.data(), golden.size()), 0);
}

TEST(PacketBuilder, FillsToMtuAndContinuesSequence) {
  PacketBuilder b(make_session("TEST"), 100);
  TrackUpdate u;
  int added = 0;
  while (b.add(u)) ++added;
  EXPECT_EQ(added, 22);
  EXPECT_EQ(b.next_seq(), 122u);
  const auto pkt = b.finish();
  EXPECT_EQ(pkt.size(), kMaxUdpPayload);
  const auto p = parse_packet(pkt);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->header.seq, 100u);
  EXPECT_EQ(p->header.count, 22u);
  // Next packet continues the sequence.
  ASSERT_TRUE(b.add(u));
  const auto p2 = parse_packet(b.finish());
  ASSERT_TRUE(p2);
  EXPECT_EQ(p2->header.seq, 122u);
}

TEST(PacketBuilder, HeartbeatAndEndOfSession) {
  PacketBuilder b(make_session("TEST"), 7);
  const auto hb = parse_packet(b.heartbeat());
  ASSERT_TRUE(hb);
  EXPECT_EQ(hb->kind, PacketKind::kHeartbeat);
  EXPECT_EQ(hb->header.seq, 7u);
  const auto eos = parse_packet(b.heartbeat(true));
  ASSERT_TRUE(eos);
  EXPECT_EQ(eos->kind, PacketKind::kEndOfSession);
}

TEST(ParsePacket, RejectsMalformedFraming) {
  const auto golden = read_golden();
  ParseError err{};
  // Too short for a header.
  EXPECT_FALSE(parse_packet(std::span(golden.data(), 19), &err));
  EXPECT_EQ(err, ParseError::kTooShort);
  // Every truncation point inside the body must be rejected, never overrun.
  for (std::size_t n = 20; n < golden.size(); ++n) {
    EXPECT_FALSE(parse_packet(std::span(golden.data(), n), &err)) << n;
  }
  // Trailing garbage after the last block.
  auto extra = golden;
  extra.push_back(0);
  EXPECT_FALSE(parse_packet(extra, &err));
  EXPECT_EQ(err, ParseError::kTrailingBytes);
  // A heartbeat must not carry a body.
  PacketBuilder b(make_session("TEST"), 1);
  const auto hb = b.heartbeat();
  std::vector<std::uint8_t> hb_body(hb.begin(), hb.end());
  hb_body.push_back(1);
  EXPECT_FALSE(parse_packet(hb_body));
}

// Builds a data packet with `n` TrackDelete messages starting at `seq`.
std::vector<std::uint8_t> packet(const char* session, std::uint64_t seq, int n) {
  PacketBuilder b(make_session(session), seq);
  for (int i = 0; i < n; ++i) EXPECT_TRUE(b.add(TrackDelete{}));
  const auto s = b.finish();
  return {s.begin(), s.end()};
}

TEST(SequenceTracker, ClassifiesPackets) {
  using R = SequenceTracker::Result;
  SequenceTracker t(1);
  auto feed = [&](const std::vector<std::uint8_t>& bytes) {
    const auto p = parse_packet(bytes);
    EXPECT_TRUE(p);
    return t.on_packet(*p);
  };

  EXPECT_EQ(feed(packet("S1", 1, 3)).result, R::kInOrder);  // 1..3
  EXPECT_EQ(t.expected(), 4u);
  EXPECT_EQ(feed(packet("S1", 2, 2)).result, R::kDuplicate);  // 2..3 again

  const auto ov = feed(packet("S1", 3, 3));  // 3..5: 3 seen, 4..5 new
  EXPECT_EQ(ov.result, R::kOverlap);
  EXPECT_EQ(ov.skip, 1u);
  EXPECT_EQ(t.expected(), 6u);

  const auto gap = feed(packet("S1", 9, 2));  // 6..8 missing
  EXPECT_EQ(gap.result, R::kGap);
  EXPECT_EQ(gap.gap_from, 6u);
  EXPECT_EQ(gap.gap_to, 9u);
  EXPECT_EQ(t.expected(), 6u) << "a gap does not advance until the caller recovers it";
  t.advance_to(11);
  EXPECT_EQ(t.expected(), 11u);
  t.advance_to(5);
  EXPECT_EQ(t.expected(), 11u) << "never moves backwards";

  // Heartbeat revealing a gap when traffic stops.
  PacketBuilder hb(make_session("S1"), 14);
  const auto h = t.on_packet(*parse_packet(hb.heartbeat()));
  EXPECT_EQ(h.result, R::kHeartbeat);
  EXPECT_EQ(h.gap_from, 11u);
  EXPECT_EQ(h.gap_to, 14u);

  // A new session id means the publisher restarted. The tracker reports it
  // but changes nothing until the caller has taken a snapshot and reset().
  const std::uint64_t before = t.expected();
  EXPECT_EQ(feed(packet("S2", 1, 1)).result, R::kNewSession);
  EXPECT_EQ(feed(packet("S2", 2, 1)).result, R::kNewSession);
  EXPECT_EQ(t.expected(), before);
  t.reset(make_session("S2"), 1);
  EXPECT_EQ(feed(packet("S2", 1, 1)).result, R::kInOrder);
  // A delayed packet from the old session (UDP reordering) is recognised as
  // stale instead of flipping the tracker back to S1.
  EXPECT_EQ(feed(packet("S1", 20, 1)).result, R::kStaleSession);
  EXPECT_EQ(feed(packet("S2", 2, 1)).result, R::kInOrder);
  EXPECT_EQ(feed(packet("S3", 1, 1)).result, R::kNewSession);
}

// Found by fuzz_packet in CI: a packet whose seq + count passes 2^64 wrapped
// `expected` backwards. Sequence numbers never wrap within a session (they
// start at 1), so such a packet, like seq 0, is malformed and changes nothing.
TEST(SequenceTracker, RejectsSequenceNumbersThatWouldWrap) {
  using R = SequenceTracker::Result;
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  auto feed = [](SequenceTracker& t, const std::vector<std::uint8_t>& bytes) {
    const auto p = parse_packet(bytes);
    EXPECT_TRUE(p);
    return t.on_packet(*p).result;
  };

  SequenceTracker at_top(kMax - 1);
  EXPECT_EQ(feed(at_top, packet("S1", kMax - 1, 3)), R::kMalformed);  // would end at 2^64 + 1
  EXPECT_EQ(at_top.expected(), kMax - 1);
  EXPECT_EQ(feed(at_top, packet("S1", kMax - 1, 1)), R::kInOrder);  // ends exactly at kMax: fine
  EXPECT_EQ(at_top.expected(), kMax);

  SequenceTracker t(1);
  EXPECT_EQ(feed(t, packet("S1", 1, 1)), R::kInOrder);
  EXPECT_EQ(feed(t, packet("S1", kMax, 2)), R::kMalformed);  // not reported as a gap
  EXPECT_EQ(feed(t, packet("S1", 0, 1)), R::kMalformed);
  PacketBuilder hb(make_session("S1"), 0);
  EXPECT_EQ(t.on_packet(*parse_packet(hb.heartbeat())).result, R::kMalformed);
  EXPECT_EQ(feed(t, packet("S9", 0, 1)), R::kMalformed) << "not mistaken for a publisher restart";
  EXPECT_EQ(t.expected(), 2u);
  EXPECT_EQ(feed(t, packet("S1", 2, 1)), R::kInOrder);
}

}  // namespace
