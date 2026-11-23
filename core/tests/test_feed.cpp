// Feed building blocks: book, snapshot stream, retransmit store, recovery
// logic, publisher, fault injection.

#include <gtest/gtest.h>

#include <cctype>
#include <cstring>
#include <vector>

#include "adsb/common/rng.hpp"
#include "adsb/feed/book.hpp"
#include "adsb/feed/net.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/recovery.hpp"
#include "adsb/feed/snapshot.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;
using protocol::PacketKind;
using protocol::TrackDelete;
using protocol::TrackUpdate;

const protocol::Session kSession = protocol::make_session("TEST000001");

TrackUpdate update(std::uint32_t icao, std::int32_t alt, std::uint64_t ts = 0) {
  TrackUpdate u;
  u.icao = icao;
  u.alt_ft = alt;
  u.event_ts_ns = ts;
  u.lat_e7 = static_cast<std::int32_t>(icao * 7);
  std::memcpy(u.callsign.data(), "TEST    ", 8);
  return u;
}

std::vector<std::uint8_t> bytes_of(const TrackUpdate& u) {
  std::vector<std::uint8_t> b(TrackUpdate::kWireSize);
  protocol::write(u, b.data());
  return b;
}
std::vector<std::uint8_t> bytes_of(const TrackDelete& d) {
  std::vector<std::uint8_t> b(TrackDelete::kWireSize);
  protocol::write(d, b.data());
  return b;
}
TrackDelete del(std::uint32_t icao, std::uint64_t ts = 0) {
  TrackDelete d;
  d.icao = icao;
  d.reason = 1;
  d.event_ts_ns = ts;
  return d;
}

// ---------------------------------------------------------------------------
// FeedBook
// ---------------------------------------------------------------------------

TEST(FeedBook, AppliesUpdatesAndDeletes) {
  FeedBook b;
  EXPECT_EQ(b.apply(bytes_of(update(1, 100))), ApplyResult::kUpdate);
  EXPECT_EQ(b.apply(bytes_of(update(2, 200))), ApplyResult::kUpdate);
  EXPECT_EQ(b.apply(bytes_of(update(1, 150))), ApplyResult::kUpdate);
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b.find(1)->alt_ft, 150);
  EXPECT_EQ(b.apply(bytes_of(del(2))), ApplyResult::kDelete);
  EXPECT_EQ(b.apply(bytes_of(del(2))), ApplyResult::kDeleteUnknown);
  EXPECT_EQ(b.size(), 1u);
  EXPECT_EQ(b.find(2), nullptr);
}

TEST(FeedBook, SkipsUnknownAndRejectsShortMessages) {
  FeedBook b;
  const std::uint8_t unknown[] = {'Z', 0, 1, 2};
  EXPECT_EQ(b.apply(unknown), ApplyResult::kUnknownType);
  auto u = bytes_of(update(1, 1));
  u.pop_back();
  EXPECT_EQ(b.apply(u), ApplyResult::kMalformed);
  EXPECT_EQ(b.apply(std::span<const std::uint8_t>()), ApplyResult::kMalformed);
  // Longer is fine: later versions may append fields.
  auto longer = bytes_of(update(3, 3));
  longer.push_back(0xAB);
  EXPECT_EQ(b.apply(longer), ApplyResult::kUpdate);
  EXPECT_EQ(b.size(), 1u);
}

TEST(FeedBook, DigestIsOrderIndependentAndSensitiveToEveryByte) {
  FeedBook a, b;
  for (std::uint32_t i = 1; i <= 50; ++i) a.put(update(i, static_cast<std::int32_t>(i)));
  for (std::uint32_t i = 50; i >= 1; --i) b.put(update(i, static_cast<std::int32_t>(i)));
  EXPECT_EQ(a.digest(), b.digest());
  EXPECT_TRUE(a == b);
  TrackUpdate changed = *b.find(17);
  changed.publish_ts_ns ^= 1;  // a single bit, in a field nobody displays
  b.put(changed);
  EXPECT_NE(a.digest(), b.digest());
  EXPECT_FALSE(a == b);
  FeedBook empty1, empty2;
  EXPECT_EQ(empty1.hex_digest(), empty2.hex_digest());
  EXPECT_NE(empty1.digest(), a.digest());
}

// ---------------------------------------------------------------------------
// Snapshot stream
// ---------------------------------------------------------------------------

FeedBook sample_book(int n) {
  FeedBook b;
  Rng rng(3);
  for (int i = 0; i < n; ++i) b.put(update(static_cast<std::uint32_t>(rng.below(1 << 24)), i));
  return b;
}

TEST(Snapshot, RoundTripsInEveryChunking) {
  const FeedBook book = sample_book(40);
  std::vector<std::uint8_t> s;
  serialize_snapshot(kSession, 1234, 99, book, s);
  for (std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{66}, s.size()}) {
    SnapshotParser p;
    for (std::size_t off = 0; off < s.size(); off += chunk) {
      const std::size_t n = std::min(chunk, s.size() - off);
      const auto st = p.feed(std::span<const std::uint8_t>(s.data() + off, n));
      ASSERT_NE(st, SnapshotParser::State::kError) << p.error();
      if (off + n < s.size()) {
        ASSERT_EQ(st, SnapshotParser::State::kNeedMore);
      }
    }
    ASSERT_EQ(p.state(), SnapshotParser::State::kDone) << "chunk " << chunk;
    EXPECT_EQ(p.data().session, kSession);
    EXPECT_EQ(p.data().as_of_seq, 1234u);
    EXPECT_EQ(p.data().event_ts_ns, 99u);
    FeedBook back;
    for (const auto& u : p.data().tracks) back.put(u);
    EXPECT_TRUE(back == book);
  }
}

TEST(Snapshot, EmptyBook) {
  std::vector<std::uint8_t> s;
  serialize_snapshot(kSession, 0, 0, FeedBook{}, s);
  SnapshotParser p;
  EXPECT_EQ(p.feed(s), SnapshotParser::State::kDone);
  EXPECT_TRUE(p.data().tracks.empty());
}

TEST(Snapshot, EveryTruncationNeedsMoreAndNothingIsAcceptedAfterTheEnd) {
  std::vector<std::uint8_t> s;
  serialize_snapshot(kSession, 7, 0, sample_book(3), s);
  for (std::size_t cut = 0; cut < s.size(); ++cut) {
    SnapshotParser p;
    EXPECT_EQ(p.feed(std::span<const std::uint8_t>(s.data(), cut)), SnapshotParser::State::kNeedMore)
        << cut;
  }
  SnapshotParser p;
  std::vector<std::uint8_t> extra = s;
  extra.push_back(0);
  EXPECT_EQ(p.feed(extra), SnapshotParser::State::kError);
}

TEST(Snapshot, RejectsCorruptStreams) {
  std::vector<std::uint8_t> good;
  serialize_snapshot(kSession, 7, 0, sample_book(3), good);
  const std::size_t begin_type = 10 + 2;                       // SnapshotBegin msg_type
  const std::size_t count_at = begin_type + 4;                 // SnapshotBegin.track_count
  const std::size_t first_track = begin_type + 24 + 2;         // first TrackUpdate msg_type
  const std::size_t end_seq = good.size() - 8;                 // SnapshotEnd.as_of_seq
  auto expect_error = [&](std::vector<std::uint8_t> s, const char* what) {
    SnapshotParser p;
    EXPECT_EQ(p.feed(s), SnapshotParser::State::kError) << what;
  };
  { auto s = good; s[begin_type] = 'X'; expect_error(s, "wrong first message"); }
  { auto s = good; s[first_track] = 'D'; expect_error(s, "wrong track type"); }
  { auto s = good; s[end_seq] ^= 1; expect_error(s, "as_of mismatch"); }
  { auto s = good; s[count_at] = 2; expect_error(s, "count says 2 tracks, stream has 3"); }
  { auto s = good; std::uint32_t big = SnapshotParser::kMaxTracks + 1; std::memcpy(&s[count_at], &big, 4);
    expect_error(s, "absurd count"); }
  {
    // Two tracks with the same ICAO address.
    FeedBook b;
    b.put(update(5, 1));
    std::vector<std::uint8_t> s;
    serialize_snapshot(kSession, 1, 0, b, s);
    // Duplicate the TrackUpdate block and bump the count to 2.
    const std::vector<std::uint8_t> block(s.begin() + 36, s.begin() + 36 + 66);
    s.insert(s.begin() + 36 + 66, block.begin(), block.end());
    std::uint32_t two = 2;
    std::memcpy(&s[16], &two, 4);
    expect_error(s, "duplicate ICAO");
  }
  { auto s = good; s[10] = 0; s[11] = 0; expect_error(s, "zero-length message"); }
}

// ---------------------------------------------------------------------------
// RetransmitStore and RecoveryState
// ---------------------------------------------------------------------------

TEST(RetransmitStore, HoldsTheLastCapacityMessages) {
  RetransmitStore st(5);  // rounded up to 8
  EXPECT_EQ(st.capacity(), 8u);
  EXPECT_EQ(st.oldest(), 1u);
  EXPECT_EQ(st.next(), 1u);
  const std::uint8_t m[3] = {'Z', 0, 0};
  EXPECT_FALSE(st.add(2, m)) << "must be contiguous";
  for (std::uint64_t s = 1; s <= 20; ++s) {
    std::uint8_t msg[3] = {'Z', static_cast<std::uint8_t>(s), 0};
    ASSERT_TRUE(st.add(s, msg));
  }
  EXPECT_EQ(st.next(), 21u);
  EXPECT_EQ(st.oldest(), 13u);
  EXPECT_EQ(st.get(12), nullptr);
  EXPECT_EQ(st.get(21), nullptr);
  for (std::uint64_t s = 13; s <= 20; ++s) {
    ASSERT_NE(st.get(s), nullptr);
    EXPECT_EQ(st.get(s)->seq, s);
    EXPECT_EQ(st.get(s)->bytes[1], s);
  }
  std::uint8_t big[kMaxMessageBytes + 1] = {};
  EXPECT_FALSE(st.add(21, big));
}

struct Replies {
  std::vector<std::vector<std::uint8_t>> dgrams;
  void operator()(std::span<const std::uint8_t> d) { dgrams.emplace_back(d.begin(), d.end()); }
};

std::vector<std::uint8_t> request(const protocol::Session& s, std::uint64_t seq, std::uint16_t count) {
  protocol::RetransmitRequest r;
  r.session = s;
  r.seq = seq;
  r.count = count;
  std::vector<std::uint8_t> b(protocol::RetransmitRequest::kWireSize);
  protocol::write(r, b.data());
  return b;
}

RecoveryState make_state(std::size_t ring, std::uint64_t n, std::uint32_t max_reply = 4096) {
  RecoveryConfig cfg;
  cfg.session = kSession;
  cfg.ring_capacity = ring;
  cfg.max_reply_messages = max_reply;
  RecoveryState st(cfg);
  for (std::uint64_t s = 1; s <= n; ++s) {
    const auto b = bytes_of(update(static_cast<std::uint32_t>(s % 10 + 1), static_cast<std::int32_t>(s), s));
    st.on_message(s, b);
  }
  return st;
}

TEST(RecoveryState, ServesExactlyTheRequestedRangeAcrossPackets) {
  RecoveryState st = make_state(1024, 500);
  Replies r;
  st.handle_request(request(kSession, 100, 50), r);
  // 50 TrackUpdates = 3 packets of at most 19.
  ASSERT_EQ(r.dgrams.size(), 3u);
  std::uint64_t next = 100;
  for (const auto& d : r.dgrams) {
    ASSERT_LE(d.size(), protocol::kMaxUdpPayload);
    const auto pv = protocol::parse_packet(d);
    ASSERT_TRUE(pv);
    EXPECT_EQ(pv->header.session, kSession);
    EXPECT_EQ(pv->header.seq, next);
    pv->for_each([&](std::uint64_t seq, std::span<const std::uint8_t> m) {
      TrackUpdate u;
      protocol::read(m.data(), u);
      EXPECT_EQ(static_cast<std::uint64_t>(u.alt_ft), seq);
    });
    next += pv->header.count;
  }
  EXPECT_EQ(next, 150u);
  EXPECT_EQ(st.stats().served_messages, 50u);
}

TEST(RecoveryState, ClampsToWhatWasPublishedAndToMaxReply) {
  RecoveryState st = make_state(1024, 100, 30);
  Replies r;
  st.handle_request(request(kSession, 90, 50), r);  // only 90..100 exist
  std::uint64_t got = 0;
  for (const auto& d : r.dgrams) got += protocol::parse_packet(d)->header.count;
  EXPECT_EQ(got, 11u);
  Replies r2;
  st.handle_request(request(kSession, 1, 100), r2);  // capped at 30
  got = 0;
  for (const auto& d : r2.dgrams) got += protocol::parse_packet(d)->header.count;
  EXPECT_EQ(got, 30u);
  Replies r3;
  st.handle_request(request(kSession, 101, 5), r3);  // not published yet
  EXPECT_TRUE(r3.dgrams.empty());
  EXPECT_EQ(st.stats().unavailable, 1u);
}

TEST(RecoveryState, NaksARangeThatLeftTheRing) {
  RecoveryState st = make_state(64, 1000);
  ASSERT_EQ(st.store().oldest(), 937u);
  Replies r;
  st.handle_request(request(kSession, 900, 10), r);
  ASSERT_EQ(r.dgrams.size(), 1u);
  const auto pv = protocol::parse_packet(r.dgrams[0]);
  ASSERT_TRUE(pv);
  EXPECT_EQ(pv->kind, PacketKind::kHeartbeat);
  EXPECT_EQ(pv->header.seq, 937u) << "a NAK carries the oldest sequence number still held";
  EXPECT_EQ(st.stats().naks, 1u);
  Replies edge;
  st.handle_request(request(kSession, 936, 5), edge);  // one before the oldest: gone
  ASSERT_EQ(edge.dgrams.size(), 1u);
  EXPECT_EQ(protocol::parse_packet(edge.dgrams[0])->kind, PacketKind::kHeartbeat);
  Replies ok;
  st.handle_request(request(kSession, 937, 1), ok);
  ASSERT_EQ(ok.dgrams.size(), 1u);
  EXPECT_EQ(protocol::parse_packet(ok.dgrams[0])->kind, PacketKind::kData);
}

TEST(RecoveryState, IgnoresMalformedAndForeignRequests) {
  RecoveryState st = make_state(64, 10);
  Replies r;
  st.handle_request(request(protocol::make_session("OTHER00001"), 1, 5), r);
  auto shortreq = request(kSession, 1, 5);
  shortreq.pop_back();
  st.handle_request(shortreq, r);
  st.handle_request(request(kSession, 1, 0), r);
  st.handle_request(request(kSession, 0, 3), r);
  st.handle_request(request(kSession, ~0ULL - 2, 65535), r);  // would overflow seq + count
  EXPECT_TRUE(r.dgrams.empty());
  EXPECT_EQ(st.stats().wrong_session, 1u);
  EXPECT_EQ(st.stats().malformed_requests, 3u);
}

TEST(RecoveryState, BookAndSnapshotFollowTheLog) {
  RecoveryConfig cfg;
  cfg.session = kSession;
  RecoveryState st(cfg);
  st.on_message(1, bytes_of(update(1, 10, 111)));
  st.on_message(2, bytes_of(update(2, 20, 222)));
  st.on_message(3, bytes_of(del(1, 333)));
  st.on_message(5, bytes_of(update(9, 9)));  // out of sequence: rejected
  EXPECT_EQ(st.stats().log_errors, 1u);
  EXPECT_EQ(st.last_seq(), 3u);
  ASSERT_EQ(st.book().size(), 1u);
  std::vector<std::uint8_t> s;
  st.make_snapshot(s);
  SnapshotParser p;
  ASSERT_EQ(p.feed(s), SnapshotParser::State::kDone);
  EXPECT_EQ(p.data().as_of_seq, 3u);
  EXPECT_EQ(p.data().event_ts_ns, 333u) << "event time of the last message (a delete)";
  ASSERT_EQ(p.data().tracks.size(), 1u);
  EXPECT_EQ(p.data().tracks[0].icao, 2u);
}

TEST(RecoveryState, DrainsTheMessageLog) {
  auto log = make_message_log();
  RecoveryConfig cfg;
  cfg.session = kSession;
  RecoveryState st(cfg);
  for (std::uint64_t s = 1; s <= 100; ++s) {
    LogEntry e;
    e.seq = s;
    const auto b = bytes_of(update(static_cast<std::uint32_t>(s), 1));
    e.len = static_cast<std::uint16_t>(b.size());
    std::memcpy(e.bytes, b.data(), b.size());
    ASSERT_TRUE(log->try_push(e));
  }
  EXPECT_EQ(st.drain(*log), 100u);
  EXPECT_EQ(st.book().size(), 100u);
  EXPECT_EQ(st.drain(*log), 0u);
}

// ---------------------------------------------------------------------------
// Publisher
// ---------------------------------------------------------------------------

struct CaptureOut {
  std::vector<std::vector<std::uint8_t>> dgrams;
  bool ok = true;
  bool send(std::span<const std::uint8_t> d) noexcept {
    dgrams.emplace_back(d.begin(), d.end());
    return ok;
  }
};

std::uint64_t g_now = 0;
std::uint64_t fake_clock() { return g_now; }

PublisherConfig pub_cfg(std::uint64_t batch_ns) {
  PublisherConfig c;
  c.session = kSession;
  c.max_batch_delay_ns = batch_ns;
  c.heartbeat_interval_ns = 1'000'000'000;
  return c;
}

TEST(Publisher, ImmediateSendsOnePacketPerMessageWithConsecutiveSequenceNumbers) {
  g_now = 1000;
  CaptureOut out;
  auto log = make_message_log();
  Publisher<CaptureOut> pub(pub_cfg(0), out, log.get(), &fake_clock);
  for (std::uint32_t i = 0; i < 10; ++i) {
    g_now += 5;
    pub.on_update(update(i + 1, 1));
  }
  pub.on_delete(del(3));
  ASSERT_EQ(out.dgrams.size(), 11u);
  for (std::size_t i = 0; i < out.dgrams.size(); ++i) {
    const auto pv = protocol::parse_packet(out.dgrams[i]);
    ASSERT_TRUE(pv);
    EXPECT_EQ(pv->header.seq, i + 1);
    EXPECT_EQ(pv->header.count, 1u);
  }
  TrackUpdate first;
  protocol::read(out.dgrams[0].data() + protocol::PacketHeader::kWireSize + 2, first);
  EXPECT_EQ(first.publish_ts_ns, 1005u) << "stamped with the publisher clock";
  EXPECT_EQ(pub.next_seq(), 12u);
  EXPECT_EQ(pub.stats().messages, 11u);
  // The recovery log got the same 11 messages, same seqs, same bytes.
  LogEntry e;
  for (std::uint64_t s = 1; s <= 11; ++s) {
    ASSERT_TRUE(log->try_pop(e));
    EXPECT_EQ(e.seq, s);
    const auto pv = protocol::parse_packet(out.dgrams[s - 1]);
    pv->for_each([&](std::uint64_t, std::span<const std::uint8_t> m) {
      ASSERT_EQ(m.size(), e.len);
      EXPECT_EQ(std::memcmp(m.data(), e.bytes, e.len), 0);
    });
  }
  EXPECT_FALSE(log->try_pop(e));
}

TEST(Publisher, BatchFlushesWhenFullOrWhenTheDelayExpires) {
  g_now = 0;
  CaptureOut out;
  Publisher<CaptureOut> pub(pub_cfg(50'000), out, nullptr, &fake_clock);
  for (std::uint32_t i = 0; i < 30; ++i) pub.on_update(update(i + 1, 1));  // same instant
  ASSERT_EQ(out.dgrams.size(), 1u) << "19 TrackUpdates fill a datagram (a 20th would pass 1472 bytes)";
  EXPECT_EQ(out.dgrams[0].size(), 20u + 19u * 74u);
  EXPECT_EQ(protocol::parse_packet(out.dgrams[0])->header.count, 19u);
  g_now = 49'999;
  pub.poll();
  EXPECT_EQ(out.dgrams.size(), 1u);
  g_now = 50'000;
  pub.poll();
  ASSERT_EQ(out.dgrams.size(), 2u);
  const auto pv = protocol::parse_packet(out.dgrams[1]);
  EXPECT_EQ(pv->header.seq, 20u);
  EXPECT_EQ(pv->header.count, 11u);
  // The delay is measured from the first message of the packet.
  g_now = 100'000;
  pub.on_update(update(99, 1));
  g_now = 140'000;
  pub.on_update(update(98, 1));
  EXPECT_EQ(out.dgrams.size(), 2u);
  g_now = 150'000;
  pub.on_update(update(97, 1));  // checked on publish too
  ASSERT_EQ(out.dgrams.size(), 3u);
  EXPECT_EQ(protocol::parse_packet(out.dgrams[2])->header.count, 3u);
}

TEST(Publisher, HeartbeatsOnlyWhenQuietAndEndOfSession) {
  g_now = 0;
  CaptureOut out;
  Publisher<CaptureOut> pub(pub_cfg(0), out, nullptr, &fake_clock);
  g_now = 999'999'999;
  pub.poll();
  EXPECT_TRUE(out.dgrams.empty());
  g_now = 1'000'000'000;
  pub.poll();
  ASSERT_EQ(out.dgrams.size(), 1u);
  auto pv = protocol::parse_packet(out.dgrams[0]);
  EXPECT_EQ(pv->kind, PacketKind::kHeartbeat);
  EXPECT_EQ(pv->header.seq, 1u) << "a heartbeat carries the next sequence number";
  g_now = 1'500'000'000;
  pub.on_update(update(1, 1));  // traffic resets the heartbeat timer
  g_now = 2'400'000'000;
  pub.poll();
  EXPECT_EQ(out.dgrams.size(), 2u);
  g_now = 2'500'000'000;
  pub.poll();
  ASSERT_EQ(out.dgrams.size(), 3u);
  EXPECT_EQ(protocol::parse_packet(out.dgrams[2])->header.seq, 2u);
  EXPECT_EQ(pub.stats().heartbeats, 2u);

  pub.end_session(2);
  ASSERT_EQ(out.dgrams.size(), 5u);
  pv = protocol::parse_packet(out.dgrams[4]);
  EXPECT_EQ(pv->kind, PacketKind::kEndOfSession);
  EXPECT_EQ(pv->header.seq, 2u);
  pub.on_update(update(2, 2));  // nothing can be published after the end
  EXPECT_EQ(out.dgrams.size(), 5u);
  EXPECT_EQ(pub.stats().rejected, 1u);
  g_now = 3'400'000'000;
  pub.poll();
  EXPECT_EQ(out.dgrams.size(), 5u);
  g_now = 3'500'000'000;
  pub.poll();  // ...but the end-of-session marker keeps being repeated
  ASSERT_EQ(out.dgrams.size(), 6u);
  pv = protocol::parse_packet(out.dgrams[5]);
  EXPECT_EQ(pv->kind, PacketKind::kEndOfSession);
  EXPECT_EQ(pv->header.seq, 2u);
}

TEST(Publisher, EndOfSessionFlushesPendingBatch) {
  g_now = 0;
  CaptureOut out;
  Publisher<CaptureOut> pub(pub_cfg(1'000'000), out, nullptr, &fake_clock);
  pub.on_update(update(1, 1));
  pub.on_update(update(2, 1));
  pub.end_session(1);
  ASSERT_EQ(out.dgrams.size(), 2u);
  EXPECT_EQ(protocol::parse_packet(out.dgrams[0])->header.count, 2u);
  EXPECT_EQ(protocol::parse_packet(out.dgrams[1])->kind, PacketKind::kEndOfSession);
  EXPECT_EQ(protocol::parse_packet(out.dgrams[1])->header.seq, 3u);
}

TEST(Publisher, RejectsOversizeAndCountsSendFailures) {
  g_now = 0;
  CaptureOut out;
  out.ok = false;
  Publisher<CaptureOut> pub(pub_cfg(0), out, nullptr, &fake_clock);
  std::uint8_t big[kMaxMessageBytes + 1] = {'Z'};
  pub.publish(big, sizeof big);
  EXPECT_EQ(pub.stats().rejected, 1u);
  EXPECT_EQ(pub.next_seq(), 1u) << "a rejected message does not consume a sequence number";
  pub.on_update(update(1, 1));
  EXPECT_EQ(pub.stats().send_failures, 1u);
  EXPECT_EQ(pub.next_seq(), 2u) << "a failed send is still sequenced (recoverable by retransmit)";
}

TEST(Publisher, SessionIdsDifferOnEveryStart) {
  std::vector<protocol::Session> ids;
  for (int i = 0; i < 1000; ++i) ids.push_back(unique_session('P'));
  for (std::size_t i = 0; i < ids.size(); ++i) {
    EXPECT_EQ(ids[i][0], 'P');
    for (std::size_t k = 1; k < ids[i].size(); ++k) EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(ids[i][k])));
    for (std::size_t j = 0; j < i; ++j) ASSERT_NE(ids[i], ids[j]) << i << " vs " << j;
  }
}

TEST(LossInjector, DeterministicBurstsAtTheConfiguredRate) {
  LossConfig c;
  c.drop_rate = 0.02;
  c.drop_burst = 5;
  c.seed = 42;
  LossInjector a(c), b(c);
  std::uint64_t dropped = 0, bursts = 0;
  bool prev = false;
  for (int i = 0; i < 100'000; ++i) {
    const bool d = a.drop();
    ASSERT_EQ(d, b.drop());
    dropped += d;
    if (d && !prev) ++bursts;
    prev = d;
  }
  EXPECT_EQ(dropped, a.dropped());
  // Bursts start with p = 0.02 per non-dropped datagram: ~ 100000 * 0.02 / (1 + 0.02 * 4).
  EXPECT_NEAR(static_cast<double>(bursts), 1852.0, 200.0);
  EXPECT_GE(dropped, bursts * 5 - 10) << "each burst drops drop_burst datagrams (adjacent bursts merge)";
  LossInjector none(LossConfig{});
  for (int i = 0; i < 1000; ++i) ASSERT_FALSE(none.drop());
  LossConfig all;
  all.drop_rate = 1.0;
  all.only_first = 10;
  LossInjector first10(all);
  for (int i = 0; i < 10; ++i) ASSERT_TRUE(first10.drop()) << i;
  for (int i = 0; i < 100; ++i) ASSERT_FALSE(first10.drop()) << "injection stops after only_first";
}

}  // namespace
