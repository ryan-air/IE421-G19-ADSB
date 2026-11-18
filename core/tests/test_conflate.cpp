// Conflator (feed/conflate.hpp): the conflated feed's rules, and its
// invariant — right after every tick the conflated book holds exactly the main
// feed's aircraft with the same state — on random streams and on the real
// track engine fed synthetic traffic.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "adsb/common/build_config.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/feed/book.hpp"
#include "adsb/feed/conflate.hpp"
#include "adsb/feed/conflated_feed.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb;
using namespace adsb::feed;
using protocol::TrackDelete;
using protocol::TrackUpdate;
namespace mask = protocol::update_mask;

// Collects what the conflator sends: the messages, and the book they build.
struct Recorder {
  std::vector<TrackUpdate> updates;
  std::vector<TrackDelete> deletes;
  FeedBook book;
  void on_update(const TrackUpdate& u) {
    updates.push_back(u);
    book.put(u);
  }
  void on_delete(const TrackDelete& d) {
    deletes.push_back(d);
    book.erase(d.icao);
  }
};

TrackUpdate upd(std::uint32_t icao, std::int32_t alt, std::uint16_t m) {
  TrackUpdate u;
  u.icao = icao;
  u.alt_ft = alt;
  u.update_mask = m;
  return u;
}
TrackDelete del(std::uint32_t icao) {
  TrackDelete d;
  d.icao = icao;
  d.reason = 1;
  return d;
}

ConflatorConfig every(std::uint64_t ns, std::size_t cap = 1024) {
  ConflatorConfig c;
  c.interval_ns = ns;
  c.capacity = cap;
  return c;
}

TEST(Conflator, SeveralUpdatesBecomeOneWithTheLatestStateAndTheUnionMask) {
  Recorder out;
  Conflator<Recorder> c(every(100), out);
  c.on_update(upd(1, 1000, mask::kNewTrack | mask::kPosition));
  c.on_update(upd(1, 1100, mask::kAltitude));
  c.on_update(upd(1, 1200, mask::kAltitude | mask::kSquawk));
  EXPECT_TRUE(out.updates.empty()) << "nothing is sent between ticks";
  EXPECT_FALSE(c.poll(10)) << "the first poll only starts the clock";
  EXPECT_FALSE(c.poll(109));
  EXPECT_TRUE(c.poll(110));
  ASSERT_EQ(out.updates.size(), 1u);
  EXPECT_EQ(out.updates[0].alt_ft, 1200);
  EXPECT_EQ(out.updates[0].update_mask, mask::kNewTrack | mask::kPosition | mask::kAltitude | mask::kSquawk);
  EXPECT_EQ(c.stats().updates_in, 3u);
  EXPECT_EQ(c.stats().updates_out, 1u);
}

TEST(Conflator, UnchangedAircraftAreNotResent) {
  Recorder out;
  Conflator<Recorder> c(every(100), out);
  for (std::uint32_t i = 1; i <= 50; ++i) c.on_update(upd(i, 1000, mask::kNewTrack));
  c.tick();
  EXPECT_EQ(out.updates.size(), 50u);
  c.on_update(upd(7, 2000, mask::kAltitude));
  c.tick();
  ASSERT_EQ(out.updates.size(), 51u) << "only the one that changed";
  EXPECT_EQ(out.updates.back().icao, 7u);
  EXPECT_EQ(out.updates.back().update_mask, mask::kAltitude) << "the mask restarts after each send";
  c.tick();
  EXPECT_EQ(out.updates.size(), 51u) << "a tick with no changes sends nothing";
}

TEST(Conflator, DeletesFollowWhatWasSent) {
  Recorder out;
  Conflator<Recorder> c(every(100), out);
  // Created and deleted within one tick: never sent at all.
  c.on_update(upd(1, 1000, mask::kNewTrack));
  c.on_delete(del(1));
  c.tick();
  EXPECT_TRUE(out.updates.empty());
  EXPECT_TRUE(out.deletes.empty());
  EXPECT_EQ(c.stats().suppressed, 1u);
  EXPECT_EQ(c.tracked(), 0u);
  // Sent, then deleted: the delete is sent.
  c.on_update(upd(2, 1000, mask::kNewTrack));
  c.tick();
  c.on_delete(del(2));
  c.tick();
  ASSERT_EQ(out.deletes.size(), 1u);
  EXPECT_EQ(out.deletes[0].icao, 2u);
  EXPECT_EQ(c.tracked(), 0u);
  // Sent, deleted and re-created within one tick: the latest state is sent.
  c.on_update(upd(3, 1000, mask::kNewTrack));
  c.tick();
  c.on_delete(del(3));
  c.on_update(upd(3, 3000, mask::kNewTrack | mask::kAltitude));
  c.tick();
  EXPECT_EQ(out.deletes.size(), 1u);
  EXPECT_EQ(out.updates.back().icao, 3u);
  EXPECT_EQ(out.updates.back().alt_ft, 3000);
  ASSERT_NE(out.book.find(3), nullptr);
}

TEST(Conflator, AFullTablePassesUpdatesThroughSoNoStateIsLost) {
  Recorder out;
  Conflator<Recorder> c(every(100, 2), out);
  c.on_update(upd(1, 1, mask::kNewTrack));
  c.on_update(upd(2, 2, mask::kNewTrack));
  c.on_update(upd(3, 3, mask::kNewTrack));  // no room: sent at once
  ASSERT_EQ(out.updates.size(), 1u);
  EXPECT_EQ(out.updates[0].icao, 3u);
  c.on_delete(del(3));  // and so is its delete
  ASSERT_EQ(out.deletes.size(), 1u);
  EXPECT_EQ(c.stats().passthrough, 2u);
  c.tick();
  EXPECT_EQ(out.book.size(), 2u);
}

TEST(Conflator, ChurnThroughASmallTableLeavesNothingBehind) {
  // Many aircraft created and deleted through a 16-entry table (32 slots,
  // dense clusters), ticking every round so live + pending deletes never
  // exceed 16: nothing should pass through, and deletions must keep probe
  // chains intact (backward shift). A broken chain makes an aircraft
  // unreachable; its next update inserts a duplicate and the table leaks.
  Rng rng(3);
  Recorder out;
  Conflator<Recorder> c(every(1000, 16), out);
  FeedBook main;
  std::vector<std::uint32_t> live;
  std::uint32_t next_icao = 1;
  for (int round = 0; round < 20000; ++round) {
    if (live.size() < 15 && (live.empty() || rng.chance(0.55))) {
      const std::uint32_t icao = next_icao++ * 7919;
      live.push_back(icao);
      const TrackUpdate u = upd(icao, round, mask::kNewTrack);
      main.put(u);
      c.on_update(u);
    } else {
      const std::size_t k = rng.below(live.size());
      const std::uint32_t icao = live[k];
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
      main.erase(icao);
      c.on_delete(del(icao));
    }
    for (std::uint32_t icao : live) {
      if (rng.chance(0.3)) {
        const TrackUpdate u = upd(icao, round, mask::kAltitude);
        main.put(u);
        c.on_update(u);
      }
    }
    c.tick();
    ASSERT_EQ(c.tracked(), live.size()) << "no duplicate or orphaned entries, round " << round;
  }
  EXPECT_EQ(out.book.hex_state_digest(), main.hex_state_digest());
  EXPECT_EQ(c.stats().passthrough, 0u);
}

TEST(Conflator, OverflowNeverLosesADelete) {
  // Found by the test above in its first form: an aircraft whose update
  // passed through (table full) and that later got a slot looked "never
  // sent", so deleting it within one tick suppressed the delete while
  // subscribers still had it. Small table, lots of overflow, ticks only now
  // and then: the books must still agree after every tick.
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    Rng rng(seed);
    Recorder out;
    Conflator<Recorder> c(every(1000, 8), out);
    FeedBook main;
    for (int i = 0; i < 20000; ++i) {
      const auto icao = static_cast<std::uint32_t>(rng.below(24) + 1);  // up to 24 live, 8 entries
      if (rng.chance(0.25) && main.find(icao)) {
        main.erase(icao);
        c.on_delete(del(icao));
      } else {
        const TrackUpdate u = upd(icao, i, mask::kAltitude);
        main.put(u);
        c.on_update(u);
      }
      if (rng.chance(0.05)) {
        c.tick();
        ASSERT_EQ(out.book.hex_state_digest(), main.hex_state_digest()) << "seed " << seed << " at " << i;
      }
    }
    EXPECT_GT(c.stats().passthrough, 100u) << "overflow happened (non-vacuity)";
  }
}

TEST(ConflatedFeed, RejectsRatesThatWouldOverflowTheInterval) {
  for (double hz : {0.0, -1.0, 1e-9, 2e6}) {
    ConflatedFeed f;
    ConflatedFeedConfig cfg;
    cfg.hz = hz;
    std::string err;
    EXPECT_FALSE(f.open(cfg, &err)) << hz;
    EXPECT_FALSE(err.empty());
  }
}

TEST(Conflator, ATickThatFellBehindDoesNotBurst) {
  Recorder out;
  Conflator<Recorder> c(every(100), out);
  c.poll(0);
  c.on_update(upd(1, 1, mask::kNewTrack));
  EXPECT_TRUE(c.poll(1000));   // 9 intervals late: one tick
  EXPECT_FALSE(c.poll(1050));  // and the next one is a full interval later
  EXPECT_TRUE(c.poll(1100));
  EXPECT_EQ(c.stats().ticks, 2u);
}

TEST(Conflator, RandomStreamsKeepTheBooksEqualAfterEveryTick) {
  // A random engine-like stream (updates, deletes, re-creations, collisions
  // in a small table) against the book of every message: after each tick the
  // two books must hold the same aircraft with the same state.
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    Rng rng(seed);
    Recorder out;
    Conflator<Recorder> c(every(1000, 64), out);
    FeedBook main;
    std::uint64_t now = 0;
    for (int i = 0; i < 20'000; ++i) {
      now += rng.below(40);
      const auto icao = static_cast<std::uint32_t>(rng.below(48) + 1);  // 48 aircraft, 64 slots: collisions
      if (rng.chance(0.08) && main.find(icao)) {
        main.erase(icao);
        c.on_delete(del(icao));
      } else {
        const TrackUpdate u = upd(icao, static_cast<std::int32_t>(rng.below(40000)),
                                  static_cast<std::uint16_t>(1u << rng.below(9)));
        main.put(u);
        c.on_update(u);
      }
      if (c.poll(now)) {
        ASSERT_EQ(out.book.hex_state_digest(), main.hex_state_digest()) << "seed " << seed << " at " << i;
      }
    }
    c.tick();
    EXPECT_EQ(out.book.hex_state_digest(), main.hex_state_digest()) << "seed " << seed;
    EXPECT_LT(c.stats().updates_out, c.stats().updates_in) << "it conflated something (non-vacuity)";
    EXPECT_GT(c.stats().deletes_out, 0u);
  }
}

// Sends every engine output to the main book and to two conflators.
struct Fanout {
  FeedBook* main;
  Conflator<Recorder>* fast;  // 10 Hz
  Conflator<Recorder>* slow;  // 1 Hz
  std::uint64_t updates = 0;
  void on_update(const TrackUpdate& u) {
    ++updates;
    main->put(u);
    fast->on_update(u);
    slow->on_update(u);
  }
  void on_delete(const TrackDelete& d) {
    main->erase(d.icao);
    fast->on_delete(d);
    slow->on_delete(d);
  }
};

TEST(Conflator, RealTrafficThroughTheTrackEngine) {
  sim::SynthConfig sc;
  sc.seed = 11;
  sc.aircraft = 300;
  sc.duration_s = 60;
  sc.lines = 1;
  const auto s = sim::generate(sc);
  state::EngineConfig ec;
  ec.has_receiver = true;
  ec.receiver_lat = sc.receiver_lat;
  ec.receiver_lon = sc.receiver_lon;
  auto engine = std::make_unique<DefaultTrackEngine>(ec);
  Recorder fast_out, slow_out;
  Conflator<Recorder> fast(every(100'000'000), fast_out);   // 10 Hz, on event time
  Conflator<Recorder> slow(every(1'000'000'000), slow_out); // 1 Hz
  FeedBook main;
  Fanout sink{&main, &fast, &slow};
  std::uint64_t checks = 0;
  for (const record::Record& r : s.records) {
    if (r.type != '2' && r.type != '3') continue;
    state::FrameMeta m;
    m.rx_ns = r.rx_ns;
    m.radio_ts = r.radio_ts;
    m.line = r.line;
    engine->on_frame(r.payload, r.len, m, sink);
    if (fast.poll(r.rx_ns)) {
      ASSERT_EQ(fast_out.book.hex_state_digest(), main.hex_state_digest()) << "10 Hz, tick " << fast.stats().ticks;
      ++checks;
    }
    if (slow.poll(r.rx_ns)) {
      ASSERT_EQ(slow_out.book.hex_state_digest(), main.hex_state_digest()) << "1 Hz, tick " << slow.stats().ticks;
    }
  }
  EXPECT_GT(checks, 500u);
  // ADS-B aircraft average about 2 updates/s, so at 10 Hz there is little to
  // merge (the feed's value there is the per-aircraft rate cap); at 1 Hz
  // about half the updates go.
  const double r10 = static_cast<double>(fast.stats().updates_out) / static_cast<double>(sink.updates);
  const double r1 = static_cast<double>(slow.stats().updates_out) / static_cast<double>(sink.updates);
  EXPECT_LE(fast.stats().updates_out, sink.updates);
  EXPECT_LT(r1, 0.7) << "updates in " << sink.updates << ", out at 1 Hz " << slow.stats().updates_out;
  EXPECT_LT(r1, r10);
  // The cap: no aircraft is sent more than once per tick.
  EXPECT_LE(fast.stats().updates_out, fast.stats().ticks * static_cast<std::uint64_t>(sc.aircraft));
  std::printf("conflation on 300 aircraft, 60 s: %llu updates in; 10 Hz sends %.1f%%, 1 Hz sends %.1f%%\n",
              static_cast<unsigned long long>(sink.updates), 100 * r10, 100 * r1);
}

}  // namespace
