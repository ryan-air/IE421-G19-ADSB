#include "adsb/ingest/beast_import.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "adsb/common/int128.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using adsb::ingest::BeastFrame;
using adsb::ingest::BeastImportConfig;
using adsb::ingest::BeastImporter;
using adsb::ingest::BeastType;
using adsb::record::Record;

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kTicksPerMs = 12'000;

BeastFrame frame(std::uint64_t ts, BeastType type = BeastType::kModeSLong, std::uint8_t fill = 0x8D) {
  BeastFrame f;
  f.type = type;
  f.len = type == BeastType::kModeAC ? 2 : type == BeastType::kModeSShort ? 7 : 14;
  f.timestamp = ts & 0xFFFFFFFFFFFFULL;
  f.signal = static_cast<std::uint8_t>(ts * 7);
  for (std::size_t k = 0; k < f.len; ++k) f.payload[k] = static_cast<std::uint8_t>(fill + k);
  return f;
}

std::vector<std::uint8_t> encode(const std::vector<BeastFrame>& frames) {
  std::vector<std::uint8_t> out;
  for (const auto& f : frames) {
    std::uint8_t buf[adsb::ingest::kBeastMaxEncodedLen];
    const std::size_t n = adsb::ingest::beast_encode(f, buf);
    out.insert(out.end(), buf, buf + n);
  }
  return out;
}

std::vector<Record> import_all(const std::vector<std::uint8_t>& bytes, BeastImporter& imp,
                               std::size_t chunk = 0) {
  std::vector<Record> out;
  auto sink = [&](const Record& r) { out.push_back(r); };
  if (chunk == 0) {
    imp.feed(bytes.data(), bytes.size(), sink);
  } else {
    for (std::size_t i = 0; i < bytes.size(); i += chunk) {
      imp.feed(bytes.data() + i, std::min(chunk, bytes.size() - i), sink);
    }
  }
  return out;
}

TEST(BeastImport, TicksToNsIsTheExactFloor) {
  adsb::Rng rng(11);
  for (int i = 0; i < 200000; ++i) {
    std::uint64_t t = rng.next() >> (8 + rng.next() % 50);  // below 2^56, many magnitudes
    if (i < 10) t = static_cast<std::uint64_t>(i);
    const adsb::uint128 want = static_cast<adsb::uint128>(t) * 1'000'000'000u / 12'000'000u;
    ASSERT_EQ(BeastImporter::ticks_to_ns(t), static_cast<std::uint64_t>(want)) << t;
  }
  EXPECT_EQ(BeastImporter::ticks_to_ns(12'000'000), 1'000'000'000u);
  EXPECT_EQ(BeastImporter::ticks_to_ns(1), 83u);
}

// adsb-synth's Beast output imported back: every frame, same bytes, and rx_ns
// from its own counter. Any chunking of the input gives the same records.
TEST(BeastImport, SyntheticTrafficRoundTrips) {
  adsb::sim::SynthConfig cfg;
  cfg.aircraft = 40;
  cfg.duration_s = 20;
  cfg.seed = 3;
  const auto s = adsb::sim::generate(cfg);
  std::vector<BeastFrame> frames;
  for (const auto& r : s.records) {
    BeastFrame f;
    f.type = static_cast<BeastType>(r.type);
    f.len = r.len;
    f.signal = r.signal;
    f.timestamp = r.radio_ts & 0xFFFFFFFFFFFFULL;
    std::memcpy(f.payload, r.payload, r.len);
    frames.push_back(f);
  }
  ASSERT_GT(frames.size(), 1000u);
  const auto bytes = encode(frames);

  BeastImporter whole;
  const auto ref = import_all(bytes, whole);
  ASSERT_EQ(ref.size(), frames.size());
  // The generator sorts by its receive time, which has jitter its radio
  // counter does not: some frames are a few microseconds out of order.
  EXPECT_EQ(whole.stats().clock_resets, 0u);
  EXPECT_GT(whole.stats().out_of_order, 0u);
  EXPECT_EQ(whole.parser_stats().truncated, 0u);
  std::uint64_t latest = 0;
  for (std::size_t i = 0; i < ref.size(); ++i) {
    ASSERT_EQ(ref[i].radio_ts, frames[i].timestamp);
    ASSERT_EQ(ref[i].type, static_cast<std::uint8_t>(frames[i].type));
    ASSERT_EQ(ref[i].signal, frames[i].signal);
    ASSERT_EQ(ref[i].len, frames[i].len);
    ASSERT_EQ(0, std::memcmp(ref[i].payload, frames[i].payload, frames[i].len));
    ASSERT_EQ(ref[i].line, 0);
    latest = std::max(latest, frames[i].timestamp);
    if (whole.stats().clock_wraps == 0) {
      ASSERT_EQ(ref[i].rx_ns, BeastImporter::ticks_to_ns(latest));
    }
    if (i > 0) {
      ASSERT_GE(ref[i].rx_ns, ref[i - 1].rx_ns);
    }
  }
  for (std::size_t chunk : {1u, 2u, 3u, 7u, 21u, 64u, 997u}) {
    BeastImporter imp;
    const auto got = import_all(bytes, imp, chunk);
    ASSERT_EQ(got.size(), ref.size()) << chunk;
    for (std::size_t i = 0; i < got.size(); ++i) {
      ASSERT_EQ(got[i].rx_ns, ref[i].rx_ns) << chunk;
      ASSERT_EQ(0, std::memcmp(got[i].payload, ref[i].payload, 14)) << chunk;
    }
  }
}

TEST(BeastImport, StatusFramesAreSkippedAndModeAcIsKept) {
  const auto bytes = encode({frame(1000, BeastType::kModeAC), frame(2000, BeastType::kStatus),
                             frame(3000, BeastType::kModeSShort), frame(4000)});
  BeastImporter imp;
  const auto got = import_all(bytes, imp);
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].type, '1');
  EXPECT_EQ(got[0].len, 2);
  EXPECT_EQ(got[1].type, '2');
  EXPECT_EQ(got[2].type, '3');
  EXPECT_EQ(imp.stats().frames, 4u);
  EXPECT_EQ(imp.stats().status_skipped, 1u);
  EXPECT_EQ(imp.stats().mode_ac, 1u);
  EXPECT_EQ(imp.stats().mode_s_short, 1u);
  EXPECT_EQ(imp.stats().mode_s_long, 1u);
}

// 0x1A bytes in the counter, signal and payload are escaped on the wire.
TEST(BeastImport, EscapedBytesSurvive) {
  BeastFrame f = frame(0x1A1A1A1A1A1AULL, BeastType::kModeSLong, 0x1A);
  f.signal = 0x1A;
  for (auto& b : f.payload) b = 0x1A;
  BeastImporter imp;
  const auto got = import_all(encode({f, frame(0x1A1A1A1A1A1BULL)}), imp, 5);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].radio_ts, 0x1A1A1A1A1A1AULL);
  EXPECT_EQ(got[0].signal, 0x1A);
  for (int k = 0; k < 14; ++k) EXPECT_EQ(got[0].payload[k], 0x1A);
  EXPECT_EQ(got[1].rx_ns - got[0].rx_ns, 83u);
}

TEST(BeastImport, A48BitWrapContinuesExactly) {
  const std::uint64_t m = BeastImporter::kCounterModulus;
  const auto bytes = encode({frame(m - 2 * kTicksPerMs), frame(m - 1), frame(0), frame(5 * kTicksPerMs)});
  BeastImporter imp;
  const auto got = import_all(bytes, imp);
  ASSERT_EQ(got.size(), 4u);
  const std::uint64_t base = m - 2 * kTicksPerMs;
  EXPECT_EQ(got[1].rx_ns, BeastImporter::ticks_to_ns(base + 2 * kTicksPerMs - 1));
  EXPECT_EQ(got[2].rx_ns, BeastImporter::ticks_to_ns(base + 2 * kTicksPerMs));
  EXPECT_EQ(got[3].rx_ns, BeastImporter::ticks_to_ns(base + 7 * kTicksPerMs));
  EXPECT_EQ(got[2].radio_ts, 0u);
  EXPECT_EQ(imp.stats().clock_wraps, 1u);
  EXPECT_EQ(imp.stats().clock_resets, 0u);
}

// A restarted decoder starts its counter again: time continues reset_gap_ns
// after the last frame, and spacing after the restart is kept.
TEST(BeastImport, ABackwardsStepIsAResetThatKeepsTimeMoving) {
  const std::uint64_t t0 = 500'000 * kTicksPerMs;
  const auto bytes = encode({frame(t0), frame(t0 + 10 * kTicksPerMs), frame(7 * kTicksPerMs),
                             frame(9 * kTicksPerMs), frame(3 * kTicksPerMs), frame(4 * kTicksPerMs)});
  BeastImportConfig cfg;
  cfg.reset_gap_ns = 5 * kMs;
  cfg.reorder_ns = 1 * kMs;  // the second step back (6 ms) is a reset too
  BeastImporter imp(cfg);
  const auto got = import_all(bytes, imp);
  ASSERT_EQ(got.size(), 6u);
  EXPECT_EQ(imp.stats().clock_resets, 2u);
  EXPECT_EQ(got[2].rx_ns - got[1].rx_ns, 5 * kMs);
  EXPECT_EQ(got[3].rx_ns - got[2].rx_ns, 2 * kMs);
  EXPECT_EQ(got[4].rx_ns - got[3].rx_ns, 5 * kMs);
  EXPECT_EQ(got[5].rx_ns - got[4].rx_ns, 1 * kMs);
  EXPECT_EQ(got[2].radio_ts, 7 * kTicksPerMs);  // raw counter kept
}

// A frame slightly behind the previous one keeps the previous time, and the
// next frame is measured from the latest counter value, not the late one.
TEST(BeastImport, ASmallBackwardsStepIsReorderingNotAReset) {
  const std::uint64_t t0 = 100 * kTicksPerMs;
  BeastImportConfig cfg;
  cfg.reorder_ns = 10 * kMs;
  BeastImporter imp(cfg);
  const auto got = import_all(encode({frame(t0), frame(t0 + 5 * kTicksPerMs), frame(t0 + 1 * kTicksPerMs),
                                      frame(t0 + 6 * kTicksPerMs), frame(t0 - 20 * kTicksPerMs)}),
                              imp);
  ASSERT_EQ(got.size(), 5u);
  EXPECT_EQ(got[2].rx_ns, got[1].rx_ns);
  EXPECT_EQ(got[3].rx_ns - got[1].rx_ns, 1 * kMs);
  EXPECT_EQ(imp.stats().out_of_order, 1u);
  EXPECT_EQ(imp.stats().clock_resets, 1u);  // 26 ms back is beyond reorder_ns
  EXPECT_EQ(got[4].rx_ns - got[3].rx_ns, 1 * kMs);
}

TEST(BeastImport, EqualTimestampsAreKeptOnTheConfiguredLine) {
  BeastImportConfig cfg;
  cfg.line = 2;
  BeastImporter imp(cfg);
  const auto got = import_all(encode({frame(100), frame(100), frame(100)}), imp);
  ASSERT_EQ(got.size(), 3u);
  for (const auto& r : got) EXPECT_EQ(r.line, 2);
  EXPECT_EQ(got[0].rx_ns, got[2].rx_ns);
  EXPECT_EQ(imp.stats().clock_resets, 0u);
}

TEST(BeastImport, LongGapsAreCountedNotChanged) {
  const std::uint64_t s = 1000 * kTicksPerMs;  // one second
  BeastImporter imp;
  const auto got = import_all(encode({frame(1), frame(1 + 30 * s), frame(1 + 100 * s), frame(1 + 300 * s)}), imp);
  ASSERT_EQ(got.size(), 4u);
  EXPECT_EQ(imp.stats().long_gaps, 2u);
  EXPECT_EQ(imp.stats().longest_gap_ns, 200'000'000'000u);
  EXPECT_EQ(got[3].rx_ns - got[2].rx_ns, 200'000'000'000u);
}

// The window is measured from the input's first frame, across resets; the
// first frame at or past its end stops the import.
TEST(BeastImport, WindowSelectsAndStops) {
  std::vector<BeastFrame> frames;
  for (std::uint64_t ms = 0; ms < 40; ++ms) frames.push_back(frame(1'000'000 + ms * kTicksPerMs));
  for (std::uint64_t ms = 0; ms < 40; ++ms) frames.push_back(frame(10 + ms * kTicksPerMs));  // after a reset
  BeastImportConfig cfg;
  cfg.from_ns = 30 * kMs;
  cfg.until_ns = 50 * kMs;
  cfg.reset_gap_ns = 1 * kMs;
  cfg.reorder_ns = 0;  // the 86 ms step back is a reset
  const auto bytes = encode(frames);
  for (std::size_t chunk : {0u, 1u, 13u}) {
    BeastImporter imp(cfg);
    const auto got = import_all(bytes, imp, chunk);
    // Elapsed 30..39 ms before the reset, then 40..49 ms after it (the reset
    // puts the next frame 1 ms after the one at 39 ms).
    ASSERT_EQ(got.size(), 20u) << chunk;
    EXPECT_EQ(got.front().rx_ns - imp.stats().first_rx_ns, 30 * kMs);
    EXPECT_EQ(got.back().rx_ns - imp.stats().first_rx_ns, 49 * kMs);
    EXPECT_TRUE(imp.done());
    EXPECT_EQ(imp.stats().outside_window, 31u);  // 30 before, 1 that ended it
    EXPECT_EQ(imp.feed(bytes.data(), bytes.size(), [](const Record&) {}), 0u);
  }
}

}  // namespace
