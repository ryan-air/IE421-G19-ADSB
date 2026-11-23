// Synthetic generator and deterministic replay.

#include <gtest/gtest.h>

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "adsb/modes/decoder.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/replay/replay.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb;
namespace fs = std::filesystem;

sim::SynthConfig small_config(std::uint64_t seed = 7) {
  sim::SynthConfig c;
  c.seed = seed;
  c.aircraft = 30;
  c.duration_s = 40;
  c.lines = 2;
  return c;
}

bool same_records(const std::vector<record::Record>& a, const std::vector<record::Record>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].rx_ns != b[i].rx_ns || a[i].radio_ts != b[i].radio_ts || a[i].line != b[i].line ||
        a[i].len != b[i].len || std::memcmp(a[i].payload, b[i].payload, a[i].len) != 0) {
      return false;
    }
  }
  return true;
}

TEST(Synth, SameSeedSameOutput) {
  const auto a = sim::generate(small_config(7));
  const auto b = sim::generate(small_config(7));
  const auto c = sim::generate(small_config(8));
  EXPECT_TRUE(same_records(a.records, b.records));
  EXPECT_FALSE(same_records(a.records, c.records));
}

TEST(Synth, RealisticRatesAndOrdering) {
  sim::SynthConfig cfg = small_config();
  cfg.lines = 1;
  cfg.base_loss = 0.0;
  cfg.range_loss = 0.0;
  cfg.corrupt_rate = 0.0;
  cfg.ground_fraction = 0.0;
  cfg.surveillance_rate_hz = 0.0;
  cfg.df11_squitters = false;
  const auto s = sim::generate(cfg);
  ASSERT_FALSE(s.records.empty());
  for (std::size_t i = 1; i < s.records.size(); ++i) {
    ASSERT_LE(s.records[i - 1].rx_ns, s.records[i].rx_ns) << "records are in arrival order";
  }
  // Per aircraft: position ~2/s, velocity ~2/s, identification ~0.2/s.
  std::map<unsigned, int> per_tc;
  for (const auto& r : s.records) {
    modes::DecodedMsg m;
    ASSERT_EQ(modes::decode(r.payload, r.len, m), modes::DecodeStatus::kOk);
    ++per_tc[m.tc == 19 ? 19u : (m.tc <= 4 ? 1u : 11u)];
  }
  const double per_ac_s = cfg.aircraft * cfg.duration_s;
  EXPECT_NEAR(per_tc[11] / per_ac_s, 2.0, 0.15);
  EXPECT_NEAR(per_tc[19] / per_ac_s, 2.0, 0.15);
  EXPECT_NEAR(per_tc[1] / per_ac_s, 0.2, 0.04);
}

TEST(Synth, EveryUncorruptedFrameDecodes) {
  sim::SynthConfig cfg = small_config();
  cfg.corrupt_rate = 0.0;
  const auto s = sim::generate(cfg);
  std::set<std::uint32_t> icaos;
  for (const auto& a : s.aircraft) icaos.insert(a.icao);
  std::size_t ap = 0;
  for (const auto& r : s.records) {
    modes::DecodedMsg m;
    const auto st = modes::decode(r.payload, r.len, m);
    ASSERT_TRUE(st == modes::DecodeStatus::kOk || st == modes::DecodeStatus::kAddressParity);
    if (st == modes::DecodeStatus::kAddressParity) ++ap;
    ASSERT_TRUE(icaos.count(m.icao)) << std::hex << m.icao;
  }
  EXPECT_GT(ap, 0u) << "DF4/DF5 surveillance replies are generated";
}

TEST(Synth, TwoLinesAndRadioClocks) {
  const auto s = sim::generate(small_config());
  std::size_t per_line[2] = {0, 0};
  for (const auto& r : s.records) {
    ASSERT_LT(r.line, 2);
    ++per_line[r.line];
    ASSERT_LT(r.radio_ts, std::uint64_t{1} << 48);
  }
  EXPECT_GT(per_line[0], 1000u);
  EXPECT_GT(per_line[1], 1000u);
  // Losses are independent per line, so neither line sees everything.
  EXPECT_NE(per_line[0], per_line[1]);
}

// ---------------------------------------------------------------------------
// Replay determinism (README_UPDATES.md §11.2)
// ---------------------------------------------------------------------------

class ReplayFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (fs::temp_directory_path() / ("adsb_replay_" + std::to_string(::getpid()) + ".adsbrec")).string();
    const auto cfg = small_config(99);
    const auto s = sim::generate(cfg);
    record::RecordingWriter w;
    std::string err;
    ASSERT_TRUE(w.open(path_, sim::recording_info(cfg), &err)) << err;
    for (const auto& r : s.records) ASSERT_TRUE(w.append(r));
    ASSERT_TRUE(w.close());
    duration_s_ = cfg.duration_s;
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove(path_, ec);
  }

  std::unique_ptr<DefaultTrackEngine> engine(const record::RecordingReader& rd) {
    state::EngineConfig ec;
    ec.has_receiver = rd.info().has_receiver_position;
    ec.receiver_lat = rd.info().receiver_lat;
    ec.receiver_lon = rd.info().receiver_lon;
    return std::make_unique<DefaultTrackEngine>(ec);
  }

  std::string path_;
  double duration_s_ = 0;
};

TEST_F(ReplayFixture, HashIsIdenticalAcrossRunsSpeedsAndThreading) {
  record::RecordingReader rd;
  std::string err;
  ASSERT_TRUE(rd.open(path_, &err)) << err;
  replay::ReplayOptions max;
  max.flush_at_end = true;

  auto e1 = engine(rd);
  const auto a = replay::replay_inline(rd, *e1, max);
  auto e2 = engine(rd);
  const auto b = replay::replay_inline(rd, *e2, max);
  EXPECT_EQ(a.hash, b.hash);
  EXPECT_EQ(a.records, rd.count());
  EXPECT_GT(a.updates, 1000u);
  EXPECT_EQ(a.deletes, a.stats.tracks_created) << "flush expires every track";

  // Paced: 40 s of traffic at 200x takes ~0.2 s and must hash identically.
  replay::ReplayOptions paced = max;
  paced.speed = 200.0;
  auto e3 = engine(rd);
  const auto c = replay::replay_inline(rd, *e3, paced);
  EXPECT_EQ(c.hash, a.hash);
  EXPECT_GT(c.wall_seconds, 0.8 * duration_s_ / paced.speed) << "pacing actually waited";

  // Two-thread pipeline (SpscRing, or MutexQueue in the naive build).
  auto lat = std::make_unique<replay::PipelineLatency>();
  auto e4 = engine(rd);
  const auto d = replay::replay_pipeline(rd, *e4, max, lat.get());
  EXPECT_EQ(d.hash, a.hash);
  EXPECT_EQ(d.records, rd.count());
  EXPECT_EQ(lat->process_ns.count(), rd.count());
}

TEST_F(ReplayFixture, FlushExpiresEverythingWithLongTimeouts) {
  // Regression: the end-of-replay flush used a fixed 120 s, which left
  // tracks alive when the timeout was longer.
  record::RecordingReader rd;
  std::string err;
  ASSERT_TRUE(rd.open(path_, &err)) << err;
  state::EngineConfig ec;
  ec.track_timeout_ns = 600'000'000'000ULL;  // 10 minutes
  auto e = std::make_unique<DefaultTrackEngine>(ec);
  replay::ReplayOptions opt;
  opt.flush_at_end = true;
  const auto r = replay::replay_inline(rd, *e, opt);
  EXPECT_EQ(e->track_count(), 0u);
  EXPECT_EQ(r.deletes, r.stats.tracks_created);
}

TEST_F(ReplayFixture, CapturedOutputParsesAsFeedMessages) {
  record::RecordingReader rd;
  std::string err;
  ASSERT_TRUE(rd.open(path_, &err)) << err;
  auto e = engine(rd);
  std::vector<std::uint8_t> cap;
  const auto r = replay::replay_inline(rd, *e, {}, &cap);
  std::size_t off = 0, n = 0;
  while (off < cap.size()) {
    ASSERT_LE(off + 2, cap.size());
    std::uint16_t len;
    std::memcpy(&len, cap.data() + off, 2);
    ASSERT_LE(off + 2 + len, cap.size());
    const std::uint8_t type = cap[off + 2];
    ASSERT_TRUE((type == 'U' && len == 72) || (type == 'D' && len == 16));
    off += 2u + len;
    ++n;
  }
  EXPECT_EQ(n, r.updates + r.deletes);
}

}  // namespace
