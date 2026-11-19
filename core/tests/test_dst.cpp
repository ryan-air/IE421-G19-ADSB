// Deterministic simulation testing of the feed (feed/dst.hpp): a sweep of
// random worlds passes, every recovery path is exercised, runs replay
// exactly, and the checks catch deliberately broken worlds. adsb-dst runs the
// large sweeps (CI job `dst`).

#include <gtest/gtest.h>

#include <string>

#include "adsb/feed/dst.hpp"

namespace {

using namespace adsb::feed;

// Sanitizer and debug builds are much slower: a smaller sweep there.
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
constexpr std::uint64_t kSeeds = 1000;
#else
constexpr std::uint64_t kSeeds = 150;
#endif

TEST(Dst, ASweepOfRandomWorldsPassesAndExercisesEveryRecoveryPath) {
  dst::Totals t;
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    const dst::Result r = dst::run(dst::random_config(seed));
    ASSERT_TRUE(r.ok) << "seed " << seed << ": " << r.failure << "\n"
                      << dst::describe(dst::random_config(seed)) << "replay: adsb-dst --seed " << seed << " --trace";
    t.add(r.totals);
  }
  // Non-vacuity: the sweep reached every path, not just the happy one.
  EXPECT_GT(t.lost, 1000u);
  EXPECT_GT(t.duplicated, 100u);
  EXPECT_GT(t.gaps, 100u);
  EXPECT_GT(t.request_retries, 100u);
  EXPECT_GT(t.naks, 10u);
  EXPECT_GT(t.snapshots_loaded, 50u);
  EXPECT_GT(t.snapshot_failures, 5u);
  EXPECT_GT(t.session_changes, 50u);
  EXPECT_GT(t.stale_packets, 5u);
  EXPECT_GT(t.window_overflow, 100u);
  EXPECT_GT(t.truncated_replies, 0u);
  EXPECT_GT(t.queue_overflow, 0u);
  EXPECT_GT(t.stalls, 10u);
}

TEST(Dst, TheDeepProfilePasses) {
  for (std::uint64_t seed = 1; seed <= kSeeds / 50; ++seed) {
    const dst::Result r = dst::run(dst::random_config(seed, dst::Profile::kDeep));
    ASSERT_TRUE(r.ok) << "deep seed " << seed << ": " << r.failure;
  }
}

TEST(Dst, ARunIsAPureFunctionOfItsSeed) {
  for (std::uint64_t seed : {3u, 71u, 1234u}) {
    const dst::Config c = dst::random_config(seed);
    EXPECT_EQ(dst::describe(c), dst::describe(dst::random_config(seed)));
    const dst::Result a = dst::run(c), b = dst::run(c);
    EXPECT_EQ(a.trace_hash, b.trace_hash) << seed;
    EXPECT_EQ(a.events, b.events) << seed;
    EXPECT_EQ(a.sim_ns, b.sim_ns) << seed;
    EXPECT_EQ(a.totals.applied, b.totals.applied) << seed;
  }
  EXPECT_NE(dst::run(dst::random_config(1)).trace_hash, dst::run(dst::random_config(2)).trace_hash);
}

TEST(Dst, TraceSeesEveryEvent) {
  const dst::Config c = dst::random_config(5);
  std::uint64_t lines = 0;
  bool saw_header = false;
  dst::TraceFn fn = [&](std::uint64_t, const std::string& line) {
    ++lines;
    saw_header = saw_header || line.find("| session 1 seq") != std::string::npos;
  };
  const dst::Result r = dst::run(c, &fn);
  ASSERT_TRUE(r.ok) << r.failure;
  EXPECT_GE(lines, r.events);
  EXPECT_TRUE(saw_header) << "datagram events show their packet header";
}

// Worlds where, before SubscriberCore confirmed every session change with a
// snapshot, a late old-session packet around a restart put a subscriber
// live on the dead session (each fails within milliseconds without the fix).
TEST(Dst, SeedsThatFoundTheStaleSessionBugPass) {
  for (std::uint64_t seed : {893u, 1097u, 1826u, 1851u, 2053u, 2407u, 2806u, 3649u, 5182u, 5856u}) {
    const dst::Result r = dst::run(dst::random_config(seed));
    EXPECT_TRUE(r.ok) << "seed " << seed << ": " << r.failure;
  }
}

// Worlds where an async snapshot fetch, asked for before a session change and
// answered by a server that died since, put a subscriber on the dead session
// (before only answers to requests made in the current session could switch
// sessions). Rare: 2 in 120,000 quick worlds.
TEST(Dst, SeedsThatFoundTheStaleSnapshotBugPass) {
  for (std::uint64_t seed : {2542u, 75323u}) {
    const dst::Result r = dst::run(dst::random_config(seed));
    EXPECT_TRUE(r.ok) << "seed " << seed << ": " << r.failure;
  }
}

// The checks themselves: break something outside the code under test and
// make sure some world notices.
void expect_caught(dst::Sabotage s, const char* what) {
  int caught = 0, armed = 0;
  for (std::uint64_t seed = 1; seed <= 400 && caught < 3; ++seed) {
    const dst::Result clean = dst::run(dst::random_config(seed));
    ASSERT_TRUE(clean.ok) << clean.failure;
    const dst::Result r = dst::run(dst::random_config(seed), s);
    if (r.trace_hash != clean.trace_hash || !r.ok) ++armed;
    if (!r.ok) {
      ++caught;
      EXPECT_FALSE(r.failure.empty());
    }
  }
  EXPECT_GE(caught, 3) << what << " (armed in " << armed << " worlds)";
}

TEST(Dst, ACorruptedRetransmitIsCaught) { expect_caught(dst::Sabotage::kCorruptReply, "corrupted reply"); }

TEST(Dst, ASnapshotThatOverstatesItsSequenceIsCaught) {
  expect_caught(dst::Sabotage::kSkipSnapshotSeq, "snapshot as of one message too many");
}

}  // namespace
