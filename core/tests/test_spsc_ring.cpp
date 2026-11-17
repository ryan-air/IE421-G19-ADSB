#include "adsb/common/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

using adsb::SpscRing;

TEST(SpscRing, StartsEmpty) {
  SpscRing<int, 8> ring;
  int v = -1;
  EXPECT_FALSE(ring.try_pop(v));
  EXPECT_EQ(v, -1);
  EXPECT_EQ(ring.front(), nullptr);
  EXPECT_EQ(ring.size_approx(), 0u);
  EXPECT_TRUE(ring.empty_approx());
  EXPECT_EQ(ring.capacity(), 8u);
}

TEST(SpscRing, FifoOrderAndFull) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.try_push(i));
  EXPECT_FALSE(ring.try_push(99)) << "ring of capacity 4 must reject a 5th element";
  EXPECT_EQ(ring.size_approx(), 4u);
  for (int i = 0; i < 4; ++i) {
    int v = -1;
    ASSERT_TRUE(ring.try_pop(v));
    EXPECT_EQ(v, i);
  }
  int v = -1;
  EXPECT_FALSE(ring.try_pop(v));
}

TEST(SpscRing, WrapsAroundManyTimes) {
  SpscRing<std::uint64_t, 8> ring;
  std::uint64_t next_push = 0;
  std::uint64_t next_pop = 0;
  // Interleave pushes and pops with a varying fill level so every slot index
  // and both "cached index is stale" paths are exercised.
  for (int round = 0; round < 10'000; ++round) {
    const int pushes = round % 9;
    for (int i = 0; i < pushes; ++i) {
      if (ring.try_push(next_push)) ++next_push;
    }
    const int pops = (round * 7) % 9;
    for (int i = 0; i < pops; ++i) {
      std::uint64_t v = 0;
      if (!ring.try_pop(v)) break;
      ASSERT_EQ(v, next_pop);
      ++next_pop;
    }
    ASSERT_LE(next_push - next_pop, 8u);
  }
  std::uint64_t v = 0;
  while (ring.try_pop(v)) {
    ASSERT_EQ(v, next_pop);
    ++next_pop;
  }
  EXPECT_EQ(next_pop, next_push);
}

TEST(SpscRing, FrontAndPop) {
  SpscRing<int, 4> ring;
  ASSERT_TRUE(ring.try_push(7));
  ASSERT_TRUE(ring.try_push(8));
  const int* p = ring.front();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 7);
  EXPECT_EQ(ring.front(), p) << "front() must not consume";
  ring.pop();
  p = ring.front();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 8);
  ring.pop();
  EXPECT_EQ(ring.front(), nullptr);
}

TEST(SpscRing, BatchPushPop) {
  SpscRing<int, 8> ring;
  const std::array<int, 5> a{1, 2, 3, 4, 5};
  EXPECT_EQ(ring.try_push_n(a), 5u);
  EXPECT_EQ(ring.try_push_n(a), 3u) << "only 3 free slots remain";
  EXPECT_EQ(ring.try_push_n(a), 0u);

  std::array<int, 6> out{};
  EXPECT_EQ(ring.try_pop_n(out), 6u);
  EXPECT_EQ(out, (std::array<int, 6>{1, 2, 3, 4, 5, 1}));
  EXPECT_EQ(ring.try_pop_n(out), 2u);
  EXPECT_EQ(out[0], 2);
  EXPECT_EQ(out[1], 3);
  EXPECT_EQ(ring.try_pop_n(out), 0u);

  // Empty spans are a no-op.
  EXPECT_EQ(ring.try_push_n(std::span<const int>{}), 0u);
  EXPECT_EQ(ring.try_pop_n(std::span<int>{}), 0u);
}

TEST(SpscRing, LayoutKeepsIndicesOnSeparateCacheLines) {
  using Ring = SpscRing<std::uint64_t, 16>;
  EXPECT_GE(alignof(Ring), adsb::kCacheLine);
  // Producer line + consumer line + slots, each cache-line aligned.
  EXPECT_GE(sizeof(Ring), 2 * adsb::kCacheLine + 16 * sizeof(std::uint64_t));
}

// A 64-byte message where every word carries the same sequence number, so a
// torn (partially written) slot is detectable on the consumer side.
struct Wide {
  std::uint64_t words[8];
};
static_assert(sizeof(Wide) == 64);

// Two-thread stress test: the consumer must see every value exactly once, in
// order, with no torn slots. Run under TSan in CI to check the memory ordering.
template <std::size_t Capacity>
void run_two_thread_stress(std::uint64_t total, bool batched) {
  auto ring = std::make_unique<SpscRing<Wide, Capacity>>();
  std::atomic<bool> failed{false};

  std::thread producer([&] {
    std::mt19937_64 rng(1);
    std::uint64_t seq = 0;
    std::vector<Wide> batch;
    while (seq < total) {
      if (batched) {
        const std::size_t want = 1 + static_cast<std::size_t>(rng() % 32);
        batch.clear();
        for (std::size_t i = 0; i < want && seq + i < total; ++i) {
          Wide w{};
          for (auto& word : w.words) word = seq + i;
          batch.push_back(w);
        }
        seq += ring->try_push_n(batch);
      } else {
        Wide w{};
        for (auto& word : w.words) word = seq;
        if (ring->try_push(w)) ++seq;
      }
    }
  });

  std::uint64_t expected = 0;
  std::mt19937_64 rng(2);
  std::array<Wide, 32> buf{};
  while (expected < total && !failed.load(std::memory_order_relaxed)) {
    std::size_t got = 0;
    if (batched) {
      const std::size_t want = 1 + static_cast<std::size_t>(rng() % 32);
      got = ring->try_pop_n(std::span<Wide>(buf.data(), want));
    } else if (ring->try_pop(buf[0])) {
      got = 1;
    }
    for (std::size_t i = 0; i < got; ++i) {
      for (const auto word : buf[i].words) {
        if (word != expected) {
          failed.store(true);
          ADD_FAILURE() << "expected " << expected << " got " << word;
          break;
        }
      }
      ++expected;
    }
  }
  producer.join();
  EXPECT_FALSE(failed.load());
  EXPECT_EQ(expected, total);
  EXPECT_EQ(ring->size_approx(), 0u);
}

// Sanitizer builds run ~5-20x slower; keep the stress size moderate there.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t kStressCount = 200'000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr std::uint64_t kStressCount = 200'000;
#else
constexpr std::uint64_t kStressCount = 2'000'000;
#endif
#else
constexpr std::uint64_t kStressCount = 2'000'000;
#endif

TEST(SpscRingStress, SingleElementTinyRing) { run_two_thread_stress<2>(kStressCount, false); }
TEST(SpscRingStress, SingleElementLargeRing) { run_two_thread_stress<1024>(kStressCount, false); }
TEST(SpscRingStress, BatchedSmallRing) { run_two_thread_stress<16>(kStressCount, true); }
TEST(SpscRingStress, BatchedLargeRing) { run_two_thread_stress<4096>(kStressCount, true); }

}  // namespace
