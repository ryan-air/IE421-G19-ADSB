// SubscriberCore: the recovery protocol of spec/FeedRecovery.tla, driven
// without sockets. Hand-written scenarios for each rule, then a randomized
// simulation (loss, duplication, reordering, lost requests and replies,
// slow and failed snapshots, a small retransmit ring, publisher restarts)
// that checks the TLA+ invariants at every step and the final state
// byte for byte.

#include <gtest/gtest.h>

#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "adsb/common/rng.hpp"
#include "adsb/feed/publisher.hpp"
#include "adsb/feed/recovery.hpp"
#include "adsb/feed/subscriber.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;
using protocol::PacketBuilder;
using protocol::TrackDelete;
using protocol::TrackUpdate;
using Source = SubscriberCore::Source;
using Mode = SubscriberCore::Mode;

const protocol::Session kS1 = protocol::make_session("SESSION001");
const protocol::Session kS2 = protocol::make_session("SESSION002");

std::vector<std::uint8_t> msg_for(std::uint64_t seq) {
  // Message `seq` updates track (seq % 7) + 1 with altitude = seq, so the book
  // reveals exactly which messages were applied.
  TrackUpdate u;
  u.icao = static_cast<std::uint32_t>(seq % 7 + 1);
  u.alt_ft = static_cast<std::int32_t>(seq);
  u.event_ts_ns = seq;
  std::vector<std::uint8_t> b(TrackUpdate::kWireSize);
  protocol::write(u, b.data());
  return b;
}

// A data packet with messages [first, first + count).
std::vector<std::uint8_t> packet(const protocol::Session& s, std::uint64_t first, std::uint16_t count) {
  PacketBuilder b(s, first);
  for (std::uint64_t q = first; q < first + count; ++q) {
    const auto m = msg_for(q);
    EXPECT_TRUE(b.add(m.data(), static_cast<std::uint16_t>(m.size())));
  }
  const auto d = b.finish();
  return {d.begin(), d.end()};
}
std::vector<std::uint8_t> heartbeat(const protocol::Session& s, std::uint64_t next, bool eos = false) {
  PacketBuilder b(s, next);
  const auto d = b.heartbeat(eos);
  return {d.begin(), d.end()};
}

// The publisher's book after messages 1..n.
FeedBook reference(std::uint64_t n) {
  FeedBook b;
  for (std::uint64_t q = 1; q <= n; ++q) b.apply(msg_for(q));
  return b;
}

SnapshotData snapshot_as_of(const protocol::Session& s, std::uint64_t n) {
  SnapshotData d;
  d.session = s;
  d.as_of_seq = n;
  d.tracks = reference(n).sorted();
  return d;
}

struct RecordingIo : SubscriberIo {
  std::vector<protocol::RetransmitRequest> reqs;
  void send_request(const protocol::RetransmitRequest& r) override { reqs.push_back(r); }
};

struct SeqHandler : SubscriberHandler {
  std::vector<std::uint64_t> seqs;
  int resets = 0;
  int snapshots = 0;
  void on_message(std::uint64_t seq, std::span<const std::uint8_t>) override { seqs.push_back(seq); }
  void on_reset() override { ++resets; }
  void on_snapshot_loaded() override { ++snapshots; }
};

struct Fixture {
  RecordingIo io;
  SeqHandler h;
  SubscriberCore sub;
  explicit Fixture(SubscriberConfig cfg = {}) : sub(cfg, &io, &h) {}
  void feed(const std::vector<std::uint8_t>& d, std::uint64_t now = 0) { sub.on_datagram(d, Source::kFeed, now); }
  void reply(const std::vector<std::uint8_t>& d, std::uint64_t now = 0) {
    sub.on_datagram(d, Source::kRetransmit, now);
  }
};

TEST(Subscriber, AppliesInOrderWithoutRequests) {
  Fixture f;
  f.feed(packet(kS1, 1, 3));
  f.feed(packet(kS1, 4, 2));
  f.feed(heartbeat(kS1, 6));
  EXPECT_EQ(f.sub.expected(), 6u);
  EXPECT_TRUE(f.sub.book() == reference(5));
  EXPECT_TRUE(f.io.reqs.empty());
  EXPECT_EQ(f.h.seqs, (std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(f.sub.stats().gaps, 0u);
}

TEST(Subscriber, GapIsRequestedBufferedAndFilledByRetransmit) {
  Fixture f;
  f.feed(packet(kS1, 1, 2));
  f.feed(packet(kS1, 6, 2));  // 3..5 lost
  ASSERT_EQ(f.io.reqs.size(), 1u);
  EXPECT_EQ(f.io.reqs[0].session, kS1);
  EXPECT_EQ(f.io.reqs[0].seq, 3u);
  EXPECT_EQ(f.io.reqs[0].count, 3u) << "up to the first buffered message";
  EXPECT_EQ(f.sub.expected(), 3u);
  EXPECT_TRUE(f.sub.book() == reference(2)) << "nothing applied out of order";
  f.feed(packet(kS1, 8, 1));  // more arrives while the request is outstanding
  EXPECT_EQ(f.io.reqs.size(), 1u) << "no second request before the timeout";
  f.reply(packet(kS1, 3, 3));
  EXPECT_EQ(f.sub.expected(), 9u) << "the reply plus the buffered 6..8";
  EXPECT_TRUE(f.sub.book() == reference(8));
  EXPECT_EQ(f.h.seqs, (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6, 7, 8}));
  EXPECT_FALSE(f.sub.request_outstanding());
  EXPECT_EQ(f.sub.stats().gaps, 1u);
}

TEST(Subscriber, DuplicatesAndOverlapsAreAppliedOnce) {
  Fixture f;
  f.feed(packet(kS1, 1, 4));
  f.feed(packet(kS1, 1, 4));  // duplicate
  f.feed(packet(kS1, 3, 4));  // overlap: 3,4 seen, 5,6 new
  f.reply(packet(kS1, 2, 2)); // late retransmit
  EXPECT_EQ(f.sub.expected(), 7u);
  EXPECT_EQ(f.h.seqs, (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(f.sub.stats().duplicates, 4u + 2u + 2u);
  // A buffered message received twice is also a duplicate.
  f.feed(packet(kS1, 9, 1));
  f.feed(packet(kS1, 9, 1));
  EXPECT_EQ(f.sub.stats().buffered, 1u);
}

TEST(Subscriber, HeartbeatRevealsALostTail) {
  Fixture f;
  f.feed(packet(kS1, 1, 5));
  f.feed(heartbeat(kS1, 9));  // 6..8 were sent and lost, then traffic stopped
  ASSERT_EQ(f.io.reqs.size(), 1u);
  EXPECT_EQ(f.io.reqs[0].seq, 6u);
  EXPECT_EQ(f.io.reqs[0].count, 3u);
}

TEST(Subscriber, RetriesAfterTheTimeoutFromWhereItGot) {
  SubscriberConfig cfg;
  cfg.retransmit_timeout_ns = 1000;
  Fixture f(cfg);
  f.feed(packet(kS1, 1, 1), 0);
  f.feed(packet(kS1, 10, 1), 0);
  ASSERT_EQ(f.io.reqs.size(), 1u);  // [2, 10)
  f.sub.poll(999);
  EXPECT_EQ(f.io.reqs.size(), 1u);
  f.sub.poll(1000);                 // no answer at all: retried at the timeout
  ASSERT_EQ(f.io.reqs.size(), 2u);
  EXPECT_EQ(f.io.reqs[1].seq, 2u);
  EXPECT_EQ(f.io.reqs[1].count, 8u);
  EXPECT_EQ(f.sub.stats().request_retries, 1u);
  f.reply(packet(kS1, 2, 3), 1500);  // a short answer: 2..4 only
  ASSERT_EQ(f.io.reqs.size(), 3u) << "the rest is asked for at once, not after another timeout";
  EXPECT_EQ(f.io.reqs[2].seq, 5u);
  EXPECT_EQ(f.io.reqs[2].count, 5u);
  EXPECT_EQ(f.sub.stats().request_retries, 1u);
  EXPECT_EQ(f.sub.stats().truncated_replies, 1u);
}

// The server fills reply packets; a short one is the last of its answer. A
// truncated answer (server cap below our request) is followed up at once,
// but neither a full packet nor a duplicate of an earlier answer triggers a
// request (duplicates would otherwise feed a request storm).
TEST(Subscriber, ATruncatedAnswerIsFollowedUpAtOnceButDuplicatesAreNot) {
  SubscriberConfig cfg;
  cfg.max_request = 100;
  Fixture f(cfg);
  f.feed(packet(kS1, 1, 1), 0);
  f.feed(heartbeat(kS1, 101), 0);
  ASSERT_EQ(f.io.reqs.size(), 1u);  // [2, 101)
  f.reply(packet(kS1, 2, 19), 10);  // a full packet: the answer goes on
  EXPECT_EQ(f.io.reqs.size(), 1u);
  f.reply(packet(kS1, 21, 10), 20);  // short: the server stopped at 30 (its cap)
  ASSERT_EQ(f.io.reqs.size(), 2u);
  EXPECT_EQ(f.io.reqs[1].seq, 31u);
  f.reply(packet(kS1, 21, 10), 30);  // the same answer again (duplicated on the way)
  f.reply(packet(kS1, 2, 19), 30);
  EXPECT_EQ(f.io.reqs.size(), 2u) << "duplicates of an earlier answer do not ask again";
  f.reply(packet(kS1, 40, 5), 40);   // reordered: ahead of expected, not the end of an answer
  EXPECT_EQ(f.io.reqs.size(), 2u);
  EXPECT_EQ(f.sub.stats().truncated_replies, 1u);
}

TEST(Subscriber, NeverRequestsMoreThanTheWindowHolds) {
  SubscriberConfig cfg;
  cfg.window = 8;
  cfg.max_request = 256;
  Fixture f(cfg);
  f.feed(packet(kS1, 1, 1));
  f.feed(heartbeat(kS1, 1001));
  ASSERT_EQ(f.io.reqs.size(), 1u);
  EXPECT_EQ(f.io.reqs[0].count, 8u) << "reply packets past the window would be thrown away";
}

TEST(Subscriber, LargeGapIsRequestedInChunks) {
  SubscriberConfig cfg;
  cfg.max_request = 100;
  Fixture f(cfg);
  f.feed(packet(kS1, 1, 1));
  f.feed(heartbeat(kS1, 1001));
  ASSERT_EQ(f.io.reqs.size(), 1u);
  EXPECT_EQ(f.io.reqs[0].count, 100u);
  // Full reply packets (19 TrackUpdates each), as the server builds them.
  for (std::uint64_t q = 2; q < 102; q += 19) {
    f.reply(packet(kS1, q, static_cast<std::uint16_t>(std::min<std::uint64_t>(19, 102 - q))));
  }
  ASSERT_EQ(f.io.reqs.size(), 2u) << "the next chunk is requested as soon as one completes";
  EXPECT_EQ(f.io.reqs[1].seq, 102u);
}

TEST(Subscriber, NakSwitchesToSnapshotThenBufferedMessagesAreApplied) {
  Fixture f;
  f.feed(packet(kS1, 1, 2));
  f.feed(packet(kS1, 100, 3));  // gap 3..99
  ASSERT_EQ(f.io.reqs.size(), 1u);
  f.reply(heartbeat(kS1, 50));  // NAK: the server only holds 50 onwards
  EXPECT_EQ(f.sub.mode(), Mode::kSnapshot);
  EXPECT_EQ(f.sub.stats().naks, 1u);
  EXPECT_TRUE(f.sub.take_snapshot_request(0));
  EXPECT_FALSE(f.sub.take_snapshot_request(1)) << "one fetch at a time";
  f.feed(packet(kS1, 103, 2));  // still buffered while waiting
  f.sub.on_snapshot(snapshot_as_of(kS1, 101), 2);
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  EXPECT_EQ(f.sub.expected(), 105u) << "snapshot as of 101, then buffered 102..104";
  EXPECT_TRUE(f.sub.book() == reference(104));
  EXPECT_EQ(f.h.snapshots, 1);
}

TEST(Subscriber, StaleNakIsIgnored) {
  Fixture f;
  f.feed(packet(kS1, 1, 2));
  f.feed(packet(kS1, 10, 1));
  f.reply(packet(kS1, 3, 7));  // recovered
  f.reply(heartbeat(kS1, 5));  // a NAK for an older request arrives late
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  EXPECT_EQ(f.sub.stats().naks, 0u);
  EXPECT_EQ(f.sub.expected(), 11u);
}

TEST(Subscriber, SnapshotOlderThanCurrentStateKeepsTheState) {
  Fixture f;
  f.feed(packet(kS1, 1, 10));
  f.feed(packet(kS1, 200, 1));
  f.reply(heartbeat(kS1, 150));
  ASSERT_EQ(f.sub.mode(), Mode::kSnapshot);
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot(snapshot_as_of(kS1, 5), 0);  // older than what we hold
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  EXPECT_EQ(f.sub.expected(), 11u);
  EXPECT_TRUE(f.sub.book() == reference(10));
  EXPECT_EQ(f.h.snapshots, 0);
  EXPECT_FALSE(f.io.reqs.empty());
  EXPECT_EQ(f.io.reqs.back().seq, 11u) << "and it keeps recovering from there";
}

TEST(Subscriber, FailedSnapshotIsRetriedAfterTheRetryInterval) {
  SubscriberConfig cfg;
  cfg.snapshot_retry_ns = 1000;
  Fixture f(cfg);
  f.feed(packet(kS1, 50, 1));  // late join
  ASSERT_TRUE(f.sub.take_snapshot_request(10));
  f.sub.on_snapshot_failed(20);
  EXPECT_FALSE(f.sub.take_snapshot_request(1019));
  EXPECT_TRUE(f.sub.take_snapshot_request(1020));
  EXPECT_EQ(f.sub.stats().snapshot_failures, 1u);
  EXPECT_EQ(f.sub.stats().snapshot_requests, 2u);
}

TEST(Subscriber, JoinAtTheStartNeedsNoSnapshotLateJoinTakesOne) {
  Fixture early;
  early.feed(heartbeat(kS1, 1));
  EXPECT_EQ(early.sub.mode(), Mode::kLive);
  EXPECT_FALSE(early.sub.take_snapshot_request(0));

  Fixture second;  // even one message missed at the start means a snapshot
  second.feed(packet(kS1, 2, 1));
  EXPECT_EQ(second.sub.mode(), Mode::kSnapshot);

  Fixture late;
  late.feed(packet(kS1, 5000, 2));
  EXPECT_EQ(late.sub.mode(), Mode::kSnapshot);
  EXPECT_TRUE(late.io.reqs.empty()) << "no retransmit requests while waiting for a snapshot";
  ASSERT_TRUE(late.sub.take_snapshot_request(0));
  late.feed(packet(kS1, 5002, 1));
  late.sub.on_snapshot(snapshot_as_of(kS1, 4999), 0);
  EXPECT_EQ(late.sub.expected(), 5003u);
  EXPECT_TRUE(late.sub.book() == reference(5002));
}

TEST(Subscriber, LateJoinBuffersFarAheadOfExpected) {
  SubscriberConfig cfg;
  cfg.window = 64;
  Fixture f(cfg);
  f.feed(packet(kS1, 1'000'000, 10));  // expected is 1: far outside the window
  ASSERT_EQ(f.sub.mode(), Mode::kSnapshot);
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.feed(packet(kS1, 1'000'010, 10));
  f.sub.on_snapshot(snapshot_as_of(kS1, 1'000'004), 0);
  EXPECT_EQ(f.sub.expected(), 1'000'020u) << "messages buffered against the anchor were used";
  EXPECT_EQ(f.sub.stats().window_overflow, 0u);
}

TEST(Subscriber, PublisherRestartClearsTheBookAndDropsStalePackets) {
  Fixture f;
  f.feed(packet(kS1, 1, 19));
  ASSERT_EQ(f.sub.book().size(), 7u);
  f.feed(packet(kS2, 1, 3));  // new session from its start
  EXPECT_EQ(f.sub.session(), kS2);
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  EXPECT_EQ(f.sub.expected(), 4u);
  EXPECT_TRUE(f.sub.book() == reference(3)) << "nothing from the old session survives";
  EXPECT_EQ(f.h.resets, 2) << "first session, then the restart";
  EXPECT_TRUE(f.sub.confirming()) << "a session change is confirmed with the server, in the background";
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot(snapshot_as_of(kS2, 2), 0);  // the server agrees (its snapshot may lag)
  EXPECT_FALSE(f.sub.confirming());
  EXPECT_EQ(f.sub.expected(), 4u) << "a confirmation does not replace our state";
  EXPECT_TRUE(f.sub.book() == reference(3));
  EXPECT_EQ(f.sub.stats().sessions_confirmed, 1u);
  EXPECT_EQ(f.h.snapshots, 0);
  f.feed(packet(kS1, 21, 5));  // delayed packet from the old session
  f.reply(packet(kS1, 21, 5));
  EXPECT_EQ(f.sub.session(), kS2);
  EXPECT_EQ(f.sub.expected(), 4u);
  EXPECT_EQ(f.sub.stats().stale_packets, 2u);
  EXPECT_EQ(f.sub.stats().session_changes, 1u);
}

TEST(Subscriber, RestartSeenMidSessionTakesASnapshotOfTheNewSession) {
  Fixture f;
  f.feed(packet(kS1, 1, 5));
  f.feed(packet(kS2, 30, 1));  // missed the start of the new session
  EXPECT_EQ(f.sub.mode(), Mode::kSnapshot);
  EXPECT_TRUE(f.sub.book().empty());
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot(snapshot_as_of(kS2, 29), 0);
  EXPECT_EQ(f.sub.expected(), 31u);
  EXPECT_TRUE(f.sub.book() == reference(30));
}

TEST(Subscriber, SnapshotFromAnEvenNewerSessionIsAdopted) {
  const auto s3 = protocol::make_session("SESSION003");
  Fixture f;
  f.feed(packet(kS2, 30, 1));
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot(snapshot_as_of(s3, 7), 0);  // publisher restarted again meanwhile
  EXPECT_EQ(f.sub.session(), s3);
  EXPECT_EQ(f.sub.expected(), 8u);
  EXPECT_TRUE(f.sub.book() == reference(7));
  f.feed(packet(kS2, 31, 1));
  EXPECT_EQ(f.sub.stats().stale_packets, 1u);
}

TEST(Subscriber, PacketFromTwoSessionsAgoIsStale) {
  const auto s3 = protocol::make_session("SESSION003");
  Fixture f;
  f.feed(packet(kS1, 1, 3));
  f.feed(packet(kS2, 1, 3));
  f.feed(packet(s3, 1, 3));
  ASSERT_TRUE(f.sub.take_snapshot_request(0));  // confirms the change to s3
  f.sub.on_snapshot(snapshot_as_of(s3, 3), 0);
  f.feed(packet(kS1, 4, 1));  // very late, from two sessions back
  f.feed(packet(kS2, 4, 1));
  EXPECT_EQ(f.sub.session(), s3);
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  EXPECT_EQ(f.sub.stats().stale_packets, 2u);
  f.feed(packet(s3, 4, 1));
  EXPECT_EQ(f.sub.expected(), 5u);
}

TEST(Subscriber, StrayUnknownSessionIsUndoneByTheServersSnapshot) {
  const auto junk = protocol::make_session("JUNK000001");
  Fixture f;
  f.feed(packet(kS1, 1, 3));
  f.feed(packet(kS2, 1, 3));
  f.feed(packet(junk, 77, 1));  // corrupt or foreign packet switches us away
  ASSERT_EQ(f.sub.session(), junk);
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot(snapshot_as_of(kS2, 3), 0);  // the server says S2 is live
  EXPECT_EQ(f.sub.session(), kS2);
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  f.feed(packet(kS2, 4, 2));
  EXPECT_EQ(f.sub.expected(), 6u);
  EXPECT_TRUE(f.sub.book() == reference(5));
  f.feed(packet(junk, 78, 1));
  EXPECT_EQ(f.sub.session(), kS2) << "the stray session is now retired";
}

// Found by deterministic simulation (adsb-dst seed 71): around a restart, a
// late packet of the old session can arrive after the first packets of the
// new one. A subscriber that never saw the old session cannot tell it is
// older (session ids are opaque) and switches to it; the live session is
// then "retired" and its packets ignored forever. Every session change is
// therefore confirmed with the snapshot server, which is the live publisher.
TEST(Subscriber, LateOldSessionPacketAfterARestartIsUndoneByTheServersSnapshot) {
  Fixture f;
  f.feed(packet(kS2, 1, 3));                     // joins at the start of S2: no snapshot needed
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  EXPECT_EQ(f.sub.stats().snapshot_requests, 0u);
  f.feed(heartbeat(kS1, 1, /*eos=*/true));       // S1's end marker, delayed past S2's start
  ASSERT_EQ(f.sub.session(), kS1);
  EXPECT_TRUE(f.sub.confirming()) << "a session change must be confirmed";
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot(snapshot_as_of(kS2, 3), 0);  // the server: S2 is live
  EXPECT_EQ(f.sub.session(), kS2);
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  f.feed(packet(kS2, 4, 2));
  EXPECT_EQ(f.sub.expected(), 6u);
  EXPECT_TRUE(f.sub.book() == reference(5));
  f.feed(heartbeat(kS1, 1, true));
  EXPECT_EQ(f.sub.session(), kS2) << "S1 is retired now";
}

// The confirmation runs in the background: while the server is busy (it
// takes 16 snapshot clients; a restart makes every subscriber ask at once)
// or gone (the publisher ended and exited), gaps are still recovered and the
// end of the session is still reached.
TEST(Subscriber, AnUnconfirmedSessionChangeStillRecoversAndEnds) {
  Fixture f;
  f.feed(packet(kS1, 1, 3));
  f.feed(packet(kS2, 1, 2));
  ASSERT_TRUE(f.sub.confirming());
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot_failed(0);                   // refused
  f.feed(packet(kS2, 5, 1), 1);                  // a gap: 3..4
  ASSERT_EQ(f.io.reqs.size(), 1u) << "retransmit requests go out while unconfirmed";
  EXPECT_EQ(f.io.reqs[0].seq, 3u);
  f.reply(packet(kS2, 3, 2), 2);
  f.feed(heartbeat(kS2, 6, /*eos=*/true), 3);
  EXPECT_TRUE(f.sub.ended()) << "the end is reached without the confirmation";
  EXPECT_TRUE(f.sub.book() == reference(5));
  EXPECT_TRUE(f.sub.confirming());
  EXPECT_FALSE(f.sub.take_snapshot_request(1)) << "retried only after snapshot_retry_ns";
}

// An async fetch (the gateway's) can be answered after a session change, by
// a server that has died since: its session is unknown but not newer.
TEST(Subscriber, ASnapshotRequestedBeforeASessionChangeIsNeverAdopted) {
  const auto s3 = protocol::make_session("SESSION003");
  Fixture f;
  f.feed(packet(kS1, 50, 1));                    // late join to S1: snapshot mode
  ASSERT_TRUE(f.sub.take_snapshot_request(0));   // asked in S1
  f.feed(packet(s3, 1, 2));                      // S3 begins (change at seq 1: live, confirming)
  ASSERT_EQ(f.sub.session(), s3);
  f.sub.on_snapshot(snapshot_as_of(kS2, 9), 0);  // the answer: S2's server, gone by now
  EXPECT_EQ(f.sub.session(), s3) << "never adopted: it was asked for in S1";
  EXPECT_EQ(f.sub.expected(), 3u);
  EXPECT_EQ(f.sub.stats().snapshot_failures, 1u);
  ASSERT_TRUE(f.sub.take_snapshot_request(0)) << "the confirmation is asked for again at once";
  f.sub.on_snapshot(snapshot_as_of(s3, 2), 0);
  EXPECT_FALSE(f.sub.confirming());
  EXPECT_EQ(f.sub.session(), s3);
}

// Found by deterministic simulation (adsb-dst seed 40013996): two quick
// restarts; the subscriber, still fetching S1's snapshot, sees S3's packets
// first, then a late packet of S2 (which it never saw) and moves to S2,
// retiring S3. The S1-era fetch is then answered by S2's server, which has
// died since. It carries the session the subscriber is now in, but was asked
// for before two session changes: rejected, and the next fetch, answered by
// the live server, brings the subscriber to S3.
TEST(Subscriber, AStaleAnswerIsRejectedEvenWhenItMatchesTheCurrentSession) {
  const auto s3 = protocol::make_session("SESSION003");
  Fixture f;
  f.feed(packet(kS1, 50, 1));                   // late join to S1: snapshot mode
  ASSERT_TRUE(f.sub.take_snapshot_request(0));  // asked in S1
  f.feed(heartbeat(s3, 2, true));               // S3's end of session (one message)
  ASSERT_EQ(f.sub.session(), s3);
  f.feed(packet(kS2, 2, 1));                    // late, from the session between
  ASSERT_EQ(f.sub.session(), kS2);
  f.sub.on_snapshot(snapshot_as_of(kS2, 4), 0);  // the S1-era fetch, answered by S2's server
  EXPECT_EQ(f.sub.mode(), Mode::kSnapshot) << "not loaded: asked for two session changes ago";
  EXPECT_EQ(f.sub.stats().snapshot_failures, 1u);
  ASSERT_TRUE(f.sub.take_snapshot_request(0)) << "asked again at once";
  f.sub.on_snapshot(snapshot_as_of(s3, 1), 0);  // the live server
  EXPECT_EQ(f.sub.session(), s3);
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  f.feed(heartbeat(s3, 2, true));
  EXPECT_TRUE(f.sub.ended());
  EXPECT_TRUE(f.sub.book() == reference(1));
}

TEST(Subscriber, SnapshotRequestedBeforeARestartIsRejected) {
  Fixture f;
  f.feed(packet(kS1, 50, 1));  // late join to S1
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.feed(packet(kS2, 20, 1));  // S1 publisher restarted as S2 meanwhile
  f.sub.on_snapshot(snapshot_as_of(kS1, 49), 0);  // the old answer arrives
  EXPECT_EQ(f.sub.session(), kS2);
  EXPECT_EQ(f.sub.mode(), Mode::kSnapshot);
  EXPECT_EQ(f.sub.stats().snapshot_failures, 1u);
}

TEST(Subscriber, BoundariesNakAtExpectedAndSnapshotAtExpectedMinusOne) {
  Fixture f;
  f.feed(packet(kS1, 1, 4));
  f.feed(packet(kS1, 9, 1));
  f.reply(heartbeat(kS1, 5));  // oldest held == expected: it can still be served
  EXPECT_EQ(f.sub.mode(), Mode::kLive);
  f.reply(heartbeat(kS1, 6));
  ASSERT_EQ(f.sub.mode(), Mode::kSnapshot);
  ASSERT_TRUE(f.sub.take_snapshot_request(0));
  f.sub.on_snapshot(snapshot_as_of(kS1, 3), 0);  // as_of + 1 < expected: older, keep ours
  EXPECT_EQ(f.h.snapshots, 0);
  EXPECT_EQ(f.sub.expected(), 5u);
  f.reply(heartbeat(kS1, 6));
  ASSERT_TRUE(f.sub.take_snapshot_request(1'000'000'000));
  f.sub.on_snapshot(snapshot_as_of(kS1, 4), 0);  // as_of + 1 == expected: same state, adopted
  EXPECT_EQ(f.h.snapshots, 1);
  EXPECT_EQ(f.sub.expected(), 5u);
  EXPECT_TRUE(f.sub.book() == reference(4));
}

TEST(Subscriber, RejectsForgedSequenceJumps) {
  Fixture f;
  f.feed(packet(kS1, 1, 2));
  f.feed(heartbeat(kS1, (1ULL << 40)));  // would create a gap that can never fill
  f.feed(heartbeat(kS1, 0));
  EXPECT_EQ(f.sub.stats().malformed, 2u);
  EXPECT_EQ(f.sub.known_next(), 3u);
  EXPECT_TRUE(f.io.reqs.empty());
}

TEST(Subscriber, OversizedWindowIsClamped) {
  SubscriberConfig cfg;
  cfg.window = 3'000'000'000u;  // used to loop forever rounding up
  SubscriberCore sub(cfg);
  sub.on_datagram(packet(kS1, 1, 1), Source::kFeed, 0);
  EXPECT_EQ(sub.expected(), 2u);
}

TEST(Subscriber, EndOfSessionWaitsForTheTail) {
  Fixture f;
  f.feed(packet(kS1, 1, 5));
  f.feed(heartbeat(kS1, 9, /*eos=*/true));  // 6..8 lost
  EXPECT_FALSE(f.sub.ended());
  ASSERT_EQ(f.io.reqs.size(), 1u);
  f.reply(packet(kS1, 6, 3));
  EXPECT_TRUE(f.sub.ended());
}

TEST(Subscriber, MalformedDatagramsAreCountedAndIgnored) {
  Fixture f;
  std::vector<std::uint8_t> d = packet(kS1, 1, 2);
  d.pop_back();
  f.feed(d);
  f.feed(std::vector<std::uint8_t>(5, 0));
  EXPECT_EQ(f.sub.stats().malformed, 2u);
  EXPECT_FALSE(f.sub.has_session());
}

TEST(Subscriber, WindowOverflowIsRecoveredLater) {
  SubscriberConfig cfg;
  cfg.window = 16;
  cfg.max_request = 8;
  Fixture f(cfg);
  f.feed(packet(kS1, 1, 1));
  f.feed(packet(kS1, 40, 1));  // 39 ahead: outside a 16-message window
  EXPECT_EQ(f.sub.stats().window_overflow, 1u);
  // Fill 2..39 by retransmit, chunk by chunk; 40 must then be fetched too.
  for (int round = 0; round < 10 && f.sub.expected() <= 40; ++round) {
    const auto r = f.io.reqs.back();
    f.reply(packet(kS1, r.seq, r.count));
  }
  EXPECT_EQ(f.sub.expected(), 41u);
  EXPECT_TRUE(f.sub.book() == reference(40));
}

TEST(Subscriber, UnknownMessageTypesAreSkippedButSequenced) {
  Fixture f;
  PacketBuilder b(kS1, 1);
  const auto m1 = msg_for(1);
  const std::uint8_t future[] = {'Q', 0, 1, 2, 3};
  ASSERT_TRUE(b.add(m1.data(), static_cast<std::uint16_t>(m1.size())));
  ASSERT_TRUE(b.add(future, sizeof future));
  const auto d = b.finish();
  f.feed(std::vector<std::uint8_t>(d.begin(), d.end()));
  EXPECT_EQ(f.sub.expected(), 3u);
  EXPECT_EQ(f.sub.stats().unknown_messages, 1u);
  EXPECT_EQ(f.sub.book().size(), 1u);
}

// ---------------------------------------------------------------------------
// Randomized simulation
// ---------------------------------------------------------------------------

std::uint64_t g_sim_now = 0;
std::uint64_t sim_clock() { return g_sim_now; }

struct SimParams {
  std::uint64_t seed = 1;
  int messages = 3000;
  double drop = 0.1;
  double dup = 0.05;
  double req_drop = 0.1;
  double reply_drop = 0.1;
  double snap_fail = 0.2;
  std::size_t ring = 256;       // small, so NAKs and snapshots happen
  std::uint32_t window = 128;
  double delay = 0.02;           // chance a datagram is held back up to 200 steps
  bool outage = false;          // one long feed outage (forces gaps older than the ring)
  bool restart = false;
  bool late_join = false;
};

struct SimResult {
  SubscriberStats stats;
  bool ended = false;
};

// One end-to-end run. Returns false via gtest failures.
SimResult simulate(const SimParams& p) {
  Rng rng(p.seed);
  g_sim_now = 1;

  struct Net {
    std::deque<std::vector<std::uint8_t>> feed, replies;
    std::deque<std::vector<std::uint8_t>> requests;
    std::multimap<std::uint64_t, std::pair<Source, std::vector<std::uint8_t>>> delayed;  // release time
  } net;
  bool lossy = true;

  struct FeedOut {
    Net* net;
    bool send(std::span<const std::uint8_t> d) noexcept {
      net->feed.emplace_back(d.begin(), d.end());
      return true;
    }
  } out{&net};

  struct Io : SubscriberIo {
    Net* net;
    void send_request(const protocol::RetransmitRequest& r) override {
      std::vector<std::uint8_t> b(protocol::RetransmitRequest::kWireSize);
      protocol::write(r, b.data());
      net->requests.push_back(std::move(b));
    }
  } io;
  io.net = &net;

  // Invariant checker: messages are applied strictly in order with no
  // repeats between resets/snapshots, and the subscriber's book always equals
  // the publisher's book as of expected - 1 (TLA+ PrefixOfPublisher).
  struct Checker : SubscriberHandler {
    std::uint64_t last = 0;
    bool fresh = true;
    int violations = 0;
    void on_message(std::uint64_t seq, std::span<const std::uint8_t>) override {
      if (!fresh && seq != last + 1) ++violations;
      fresh = false;
      last = seq;
    }
    void on_reset() override { fresh = true; }
    void on_snapshot_loaded() override { fresh = true; }
  } checker;

  SubscriberConfig scfg;
  scfg.window = p.window;
  scfg.max_request = 64;
  scfg.retransmit_timeout_ns = 50;
  scfg.snapshot_retry_ns = 200;
  SubscriberCore sub(scfg, &io, &checker);

  // Publisher side, possibly restarted once.
  struct Side {
    protocol::Session session;
    std::unique_ptr<MessageLog> log = make_message_log();
    std::unique_ptr<RecoveryState> rec;
    std::unique_ptr<Publisher<FeedOut>> pub;
    std::vector<std::array<std::uint8_t, 32>> digest_at;  // digest_at[s] = book after message s
    FeedBook ref;
  };
  auto make_side = [&](const protocol::Session& s) {
    auto side = std::make_unique<Side>();
    side->session = s;
    RecoveryConfig rc;
    rc.session = s;
    rc.ring_capacity = p.ring;
    rc.max_reply_messages = 40;
    side->rec = std::make_unique<RecoveryState>(rc);
    PublisherConfig pc;
    pc.session = s;
    pc.max_batch_delay_ns = 5;
    pc.heartbeat_interval_ns = 20;
    pc.stamp_publish_ts = false;
    side->pub = std::make_unique<Publisher<FeedOut>>(pc, out, side->log.get(), &sim_clock);
    side->digest_at.push_back(FeedBook{}.digest());
    return side;
  };
  auto side = make_side(kS1);
  const int restart_at = p.restart ? static_cast<int>(rng.below(static_cast<std::uint64_t>(p.messages))) : -1;
  const int outage_at = p.outage ? static_cast<int>(rng.below(static_cast<std::uint64_t>(p.messages / 2))) : -1;
  const int join_at =
      p.late_join ? p.messages / 4 + static_cast<int>(rng.below(static_cast<std::uint64_t>(p.messages / 4))) : 0;

  std::optional<std::pair<SnapshotData, std::uint64_t>> pending_snapshot;  // data, deliver-at
  int published = 0;
  int violations_prefix = 0;

  auto deliver_one = [&](std::deque<std::vector<std::uint8_t>>& q, Source src, bool joined, bool deaf) {
    if (q.empty()) return;
    // Reorder: pick one of the first few in flight.
    const std::size_t i = rng.below(std::min<std::size_t>(q.size(), 4));
    std::vector<std::uint8_t> d = q[i];
    const double drop = src == Source::kFeed ? p.drop : p.reply_drop;
    const bool dup = lossy && rng.chance(p.dup);
    if (!dup) q.erase(q.begin() + static_cast<std::ptrdiff_t>(i));
    if (lossy && rng.chance(drop)) return;
    if ((!joined || deaf) && src == Source::kFeed) return;  // not listening
    if (lossy && rng.chance(p.delay)) {  // held back: arrives much later, far out of order
      net.delayed.emplace(g_sim_now + 1 + rng.below(200), std::make_pair(src, std::move(d)));
      return;
    }
    sub.on_datagram(d, src, g_sim_now);
  };
  std::uint64_t checked_expected = 0;
  const protocol::Session* checked_session = nullptr;

  for (int step = 0; step < 400'000; ++step) {
    ++g_sim_now;
    const bool joined = published >= join_at;
    // Publisher: a few messages per step, random mix of updates and deletes.
    if (published < p.messages && rng.chance(0.6)) {
      if (published == restart_at) {
        side->pub->end_session(1);
        side = make_side(kS2);
      }
      const auto icao = static_cast<std::uint32_t>(rng.below(40) + 1);
      std::vector<std::uint8_t> m;
      if (rng.chance(0.15)) {
        TrackDelete d;
        d.icao = icao;
        d.event_ts_ns = g_sim_now;
        m.resize(TrackDelete::kWireSize);
        protocol::write(d, m.data());
      } else {
        TrackUpdate u;
        u.icao = icao;
        u.alt_ft = static_cast<std::int32_t>(rng.below(40000));
        u.event_ts_ns = g_sim_now;
        u.publish_ts_ns = static_cast<std::uint64_t>(published);
        m.resize(TrackUpdate::kWireSize);
        protocol::write(u, m.data());
      }
      side->pub->publish(m.data(), static_cast<std::uint16_t>(m.size()));
      side->ref.apply(m);
      side->digest_at.push_back(side->ref.digest());
      ++published;
      if (published == p.messages) {
        // Loss eventually ceases (TLA+ fairness): stop injecting faults so
        // the run must converge, then end the session.
        lossy = false;
        side->pub->end_session(1);
      }
    }
    side->pub->poll();
    side->rec->drain(*side->log);

    // Network.
    const bool deaf = outage_at >= 0 && published >= outage_at && published < outage_at + 600;
    deliver_one(net.feed, Source::kFeed, joined, deaf);
    while (!net.delayed.empty() && net.delayed.begin()->first <= g_sim_now) {
      auto node = net.delayed.extract(net.delayed.begin());
      sub.on_datagram(node.mapped().second, node.mapped().first, g_sim_now);
    }
    if (!net.requests.empty()) {
      std::vector<std::uint8_t> r = net.requests.front();
      net.requests.pop_front();
      if (!(lossy && rng.chance(p.req_drop))) {
        side->rec->handle_request(r, [&](std::span<const std::uint8_t> d) {
          net.replies.emplace_back(d.begin(), d.end());
        });
      }
    }
    deliver_one(net.replies, Source::kRetransmit, joined, false);

    // Snapshots: taken at request time, delivered a few steps later.
    sub.poll(g_sim_now);
    if (!pending_snapshot && sub.take_snapshot_request(g_sim_now)) {
      if (lossy && rng.chance(p.snap_fail)) {
        sub.on_snapshot_failed(g_sim_now);
      } else {
        std::vector<std::uint8_t> s;
        side->rec->make_snapshot(s);
        SnapshotParser parser;
        EXPECT_EQ(parser.feed(s), SnapshotParser::State::kDone);
        pending_snapshot.emplace(parser.data(), g_sim_now + rng.below(30));
      }
    }
    if (pending_snapshot && g_sim_now >= pending_snapshot->second) {
      sub.on_snapshot(pending_snapshot->first, g_sim_now);
      pending_snapshot.reset();
    }

    // PrefixOfPublisher: in the current session, the book is exactly the
    // publisher's book after message expected - 1.
    // (Checked whenever `expected` moves; hashing every step is too slow.)
    if (sub.has_session() && sub.session() == side->session && sub.mode() == Mode::kLive &&
        (sub.expected() != checked_expected || checked_session != &side->session)) {
      checked_expected = sub.expected();
      checked_session = &side->session;
      const std::uint64_t applied = sub.expected() - 1;
      if (applied >= side->digest_at.size() || sub.book().digest() != side->digest_at[applied]) {
        ++violations_prefix;
      }
    }
    if (published == p.messages && sub.ended() && sub.session() == side->session &&
        net.delayed.empty()) {
      break;
    }
  }

  EXPECT_EQ(checker.violations, 0) << "seed " << p.seed << ": out-of-order apply";
  EXPECT_EQ(violations_prefix, 0) << "seed " << p.seed << ": book diverged from the publisher";
  SimResult res;
  res.stats = sub.stats();
  res.ended = sub.ended();
  EXPECT_TRUE(res.ended) << "seed " << p.seed << ": did not converge (expected " << sub.expected()
                         << ", known " << sub.known_next() << ")";
  EXPECT_EQ(sub.session(), side->session) << "seed " << p.seed;
  EXPECT_TRUE(sub.book() == side->rec->book()) << "seed " << p.seed << ": final book differs";
  EXPECT_EQ(sub.book().digest(), side->ref.digest()) << "seed " << p.seed;
  return res;
}

TEST(SubscriberSimulation, ConvergesByteForByteUnderLossDuplicationAndReordering) {
  SubscriberStats total{};
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    SimParams p;
    p.seed = seed;
    p.messages = 2000;
    p.outage = seed % 2 == 0;
    const SimResult r = simulate(p);
    total.requests += r.stats.requests;
    total.naks += r.stats.naks;
    total.snapshots_loaded += r.stats.snapshots_loaded;
    total.duplicates += r.stats.duplicates;
    total.request_retries += r.stats.request_retries;
    if (HasFailure()) return;
  }
  // Non-vacuity: every recovery path was exercised.
  EXPECT_GT(total.requests, 100u);
  EXPECT_GT(total.request_retries, 10u);
  EXPECT_GT(total.naks, 5u);
  EXPECT_GT(total.snapshots_loaded, 5u);
  EXPECT_GT(total.duplicates, 100u);
}

TEST(SubscriberSimulation, ConvergesAcrossAPublisherRestart) {
  std::uint64_t session_changes = 0, stale = 0;
  for (std::uint64_t seed = 100; seed < 120; ++seed) {
    SimParams p;
    p.seed = seed;
    p.messages = 2000;
    p.delay = 0.1;
    p.restart = true;
    const SimResult r = simulate(p);
    session_changes += r.stats.session_changes;
    stale += r.stats.stale_packets;
    if (HasFailure()) return;
  }
  EXPECT_EQ(session_changes, 20u);
  EXPECT_GT(stale, 0u) << "reordering delivered old-session packets after the switch";
}

TEST(SubscriberSimulation, LateJoinersConverge) {
  for (std::uint64_t seed = 200; seed < 215; ++seed) {
    SimParams p;
    p.seed = seed;
    p.messages = 2000;
    p.late_join = true;
    const SimResult r = simulate(p);
    EXPECT_GE(r.stats.snapshots_loaded, 1u) << seed;
    if (HasFailure()) return;
  }
}

TEST(SubscriberSimulation, HeavyLossWithALargeRingNeverNeedsASnapshot) {
  for (std::uint64_t seed = 300; seed < 310; ++seed) {
    SimParams p;
    p.seed = seed;
    p.drop = 0.3;
    p.ring = 1u << 16;
    p.window = 1u << 14;
    const SimResult r = simulate(p);
    EXPECT_EQ(r.stats.naks, 0u) << seed;
    // At most one snapshot: if message 1 itself is lost, the first packet the
    // subscriber sees is mid-session and it joins by snapshot.
    EXPECT_LE(r.stats.snapshots_loaded, 1u) << seed;
    if (HasFailure()) return;
  }
}

}  // namespace
