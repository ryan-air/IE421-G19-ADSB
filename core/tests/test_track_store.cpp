#include "adsb/state/track_store.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "adsb/common/rng.hpp"

namespace {

using namespace adsb::state;

TEST(FlatTrackStore, InsertFindErase) {
  auto s = std::make_unique<FlatTrackStore<16>>();
  EXPECT_FALSE(s->find(0x123456));
  auto t = s->insert(0x123456);
  ASSERT_TRUE(t);
  EXPECT_EQ(t.hot->icao, 0x123456u);
  t.hot->alt_ft = 1234;
  auto f = s->find(0x123456);
  ASSERT_TRUE(f);
  EXPECT_EQ(f.hot, t.hot);
  EXPECT_EQ(f.hot->alt_ft, 1234);
  EXPECT_EQ(s->size(), 1u);
  EXPECT_TRUE(s->erase(0x123456));
  EXPECT_FALSE(s->erase(0x123456));
  EXPECT_FALSE(s->find(0x123456));
  EXPECT_EQ(s->size(), 0u);
}

TEST(FlatTrackStore, FullCapacityAndReuse) {
  auto s = std::make_unique<FlatTrackStore<64>>();
  for (std::uint32_t i = 0; i < 64; ++i) ASSERT_TRUE(s->insert(0xA00000 + i)) << i;
  EXPECT_FALSE(s->insert(0xB00000)) << "full store must refuse, not overwrite";
  for (std::uint32_t i = 0; i < 64; ++i) ASSERT_TRUE(s->find(0xA00000 + i)) << i;
  ASSERT_TRUE(s->erase(0xA00010));
  auto t = s->insert(0xB00000);
  ASSERT_TRUE(t);
  EXPECT_EQ(t.hot->alt_ft, 0) << "a reused slot starts clean";
}

// Randomized differential test against std::unordered_map, with heavy churn
// so backward-shift deletion is exercised across wrap-around and long chains.
template <std::size_t Cap>
void differential(std::uint64_t seed, std::uint32_t key_space) {
  auto s = std::make_unique<FlatTrackStore<Cap>>();
  std::unordered_map<std::uint32_t, std::int32_t> ref;
  adsb::Rng rng(seed);
  for (int op = 0; op < 300'000; ++op) {
    const auto key = static_cast<std::uint32_t>(rng.below(key_space));
    const auto r = rng.below(10);
    if (r < 5) {
      auto t = s->find(key);
      const auto it = ref.find(key);
      ASSERT_EQ(static_cast<bool>(t), it != ref.end()) << "op " << op;
      if (t) {
        ASSERT_EQ(t.hot->alt_ft, it->second);
      }
      if (!t && ref.size() < Cap) {
        auto n = s->insert(key);
        ASSERT_TRUE(n);
        n.hot->alt_ft = static_cast<std::int32_t>(op);
        ref[key] = static_cast<std::int32_t>(op);
      }
    } else {
      ASSERT_EQ(s->erase(key), ref.erase(key) == 1) << "op " << op;
    }
    ASSERT_EQ(s->size(), ref.size());
  }
  // Every surviving key is still reachable and iteration sees each exactly once.
  std::size_t seen = 0;
  s->for_each([&](TrackRef t) {
    ++seen;
    ASSERT_EQ(ref.count(t.hot->icao), 1u);
  });
  EXPECT_EQ(seen, ref.size());
}

TEST(FlatTrackStore, DifferentialAgainstUnorderedMapSmall) { differential<16>(1, 64); }
TEST(FlatTrackStore, DifferentialAgainstUnorderedMapMedium) { differential<1024>(2, 4096); }
TEST(FlatTrackStore, DifferentialClusteredKeys) {
  // Sequential ICAO blocks (as allocated by country) are the realistic worst case.
  differential<256>(3, 300);
}

TEST(FlatTrackStore, ProbeLengthsStayShortAtFullLoad) {
  auto s = std::make_unique<FlatTrackStore<8192>>();
  adsb::Rng rng(5);
  // Realistic addresses: a dense US block plus scattered others.
  std::uint32_t n = 0;
  while (s->size() < 8192) {
    const auto key = (n % 2 == 0) ? 0xA00000 + n : static_cast<std::uint32_t>(rng.below(0xFFFFFF));
    if (!s->find(key)) {
      ASSERT_TRUE(s->insert(key));
    }
    ++n;
  }
  // Load factor 0.5 in the index: chains must stay short.
  EXPECT_LT(s->max_probe_length(), 64u);
}

TEST(NaiveTrackStore, SameInterface) {
  NaiveTrackStore s(4);
  for (std::uint32_t i = 0; i < 4; ++i) ASSERT_TRUE(s.insert(i + 1));
  EXPECT_FALSE(s.insert(99)) << "capacity is enforced for a fair comparison";
  EXPECT_TRUE(s.find(3));
  EXPECT_TRUE(s.erase(3));
  EXPECT_FALSE(s.find(3));
}

}  // namespace
