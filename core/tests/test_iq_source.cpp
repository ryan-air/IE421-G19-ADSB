// Tests for IQ ingest: the block ring (ingest/iq_ring.hpp), the
// demodulator's gap handling (Demodulator::skip), and IqSource
// (ingest/iq_source.hpp), which feeds adsb-live from IQ samples.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "adsb/common/clock.hpp"
#include "adsb/demod/demod.hpp"
#include "adsb/ingest/iq_ring.hpp"
#include "adsb/ingest/iq_source.hpp"
#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb;
using adsb::ingest::IqBlock;
using adsb::ingest::IqBlockRing;
using adsb::ingest::IqSource;
using adsb::ingest::IqSourceConfig;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct Scenario {
  sim::IqPlan plan;
  std::vector<std::uint8_t> cu8;
};

Scenario scenario(int aircraft, double seconds, std::uint64_t seed) {
  sim::SynthConfig syn;
  syn.seed = seed;
  syn.aircraft = aircraft;
  syn.duration_s = seconds;
  syn.keep_transmissions = true;
  const auto traffic = sim::generate(syn);
  sim::IqConfig cfg;
  cfg.seed = seed;
  Scenario s;
  s.plan = sim::plan_iq(syn, traffic, cfg);
  s.cu8.resize(s.plan.total_samples * 2);
  std::vector<float> chunk(2 * sim::kIqChunkSamples);
  for (std::uint64_t c = 0; c * sim::kIqChunkSamples < s.plan.total_samples; ++c) {
    const std::size_t n = sim::render_iq_chunk(s.plan, cfg, c, chunk.data());
    sim::quantize_iq(chunk.data(), n, sim::IqFormat::kCu8, s.cu8.data() + c * sim::kIqChunkSamples * 2);
  }
  return s;
}

std::string temp_file(const std::string& name, const std::vector<std::uint8_t>& bytes) {
  const std::string path = ::testing::TempDir() + "adsb_iq_" + name + "_" + std::to_string(::getpid());
  FILE* f = std::fopen(path.c_str(), "wb");
  EXPECT_NE(f, nullptr);
  if (f) {
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
  }
  return path;
}

std::string hex(const std::uint8_t* b, int len) {
  std::string s;
  char buf[3];
  for (int i = 0; i < len; ++i) {
    std::snprintf(buf, sizeof buf, "%02X", b[i]);
    s += buf;
  }
  return s;
}

// The demodulator alone over the same samples, in the same blocks.
std::vector<demod::DemodFrame> direct(const std::vector<std::uint8_t>& cu8, std::size_t block) {
  demod::DemodConfig c;
  c.block_samples = std::max<std::size_t>(block, 1024);
  demod::Demodulator d(c);
  std::vector<demod::DemodFrame> out;
  const std::size_t n = cu8.size() / 2;
  for (std::size_t off = 0; off < n; off += block) {
    const auto f = d.feed(cu8.data() + 2 * off, std::min(block, n - off), demod::SampleFormat::kCu8);
    out.insert(out.end(), f.begin(), f.end());
  }
  const auto f = d.flush();
  out.insert(out.end(), f.begin(), f.end());
  return out;
}

// Truth message with these bytes starting within 4 samples of `pos`.
bool in_truth(const sim::IqPlan& plan, const std::uint8_t* bytes, int len, double pos) {
  for (const auto& m : plan.messages) {
    if (m.len == len && std::memcmp(m.bytes, bytes, static_cast<std::size_t>(len)) == 0 && std::fabs(m.sample_pos - pos) < 4.0) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// IqBlockRing
// ---------------------------------------------------------------------------

TEST(IqBlockRing, FillsUpThenRefusesAndFreesInOrder) {
  IqBlockRing ring(4, 16);
  EXPECT_EQ(ring.peek(), nullptr);
  for (int i = 0; i < 4; ++i) {
    IqBlock* b = ring.acquire();
    ASSERT_NE(b, nullptr);
    std::memset(b->data, i, 16);
    b->first_sample = static_cast<std::uint64_t>(i) * 8;
    b->samples = 8;
    ring.commit();
  }
  EXPECT_EQ(ring.acquire(), nullptr);  // full: the producer drops
  EXPECT_EQ(ring.depth_approx(), 4u);
  for (int i = 0; i < 4; ++i) {
    const IqBlock* b = ring.peek();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->first_sample, static_cast<std::uint64_t>(i) * 8);
    EXPECT_EQ(b->data[15], i);
    ring.release();
    if (i == 0) {
      EXPECT_NE(ring.acquire(), nullptr);  // a slot is free again
    }
  }
  EXPECT_EQ(ring.peek(), nullptr);
}

// Producer and consumer threads: every committed block arrives intact and in
// order, every acquire failure is a drop, and nothing else is lost.
TEST(IqBlockRing, ThreadsSeeWholeBlocksInOrder) {
  IqBlockRing ring(8, 256);
  constexpr std::uint64_t kBlocks = 200000;
  std::atomic<bool> done{false};
  std::uint64_t dropped = 0;
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kBlocks; ++i) {
      IqBlock* b = ring.acquire();
      if (!b) {
        ++dropped;
        continue;
      }
      for (int k = 0; k < 256; ++k) b->data[k] = static_cast<std::uint8_t>(i * 31 + static_cast<std::uint64_t>(k));
      b->first_sample = i;
      b->samples = 128;
      ring.commit();
    }
    done.store(true, std::memory_order_release);
  });
  std::uint64_t got = 0, last = 0, bad = 0;
  bool first = true;
  for (;;) {
    const IqBlock* b = ring.peek();
    if (!b) {
      if (done.load(std::memory_order_acquire) && !ring.peek()) break;
      continue;
    }
    for (int k = 0; k < 256; ++k) bad += b->data[k] != static_cast<std::uint8_t>(b->first_sample * 31 + static_cast<std::uint64_t>(k));
    if (!first && b->first_sample <= last) ++bad;
    first = false;
    last = b->first_sample;
    ++got;
    ring.release();
  }
  producer.join();
  EXPECT_EQ(bad, 0u);
  EXPECT_EQ(got + dropped, kBlocks);
  EXPECT_GT(got, 0u);
}

// ---------------------------------------------------------------------------
// Demodulator::skip
// ---------------------------------------------------------------------------

// After a gap of a length that is not a multiple of the 16-sample window,
// windows no longer line up with the 1024-sample noise blocks: the floor
// (and everything decided from it) must still not depend on the split.
TEST(DemodSkip, AnUnalignedGapKeepsTheOutputIndependentOfTheSplit) {
  const Scenario sc = scenario(40, 1.0, 60);
  const std::size_t n = sc.cu8.size() / 2, gap_at = 100000, gap = 1007;
  auto run = [&](std::size_t chunk) {
    demod::DemodConfig c;
    c.block_samples = 65536;
    demod::Demodulator d(c);
    std::vector<demod::DemodFrame> out;
    auto take = [&](std::span<const demod::DemodFrame> f) { out.insert(out.end(), f.begin(), f.end()); };
    for (std::size_t off = 0; off < gap_at; off += chunk) take(d.feed(sc.cu8.data() + 2 * off, std::min(chunk, gap_at - off), demod::SampleFormat::kCu8));
    take(d.skip(gap));
    for (std::size_t off = gap_at + gap; off < n; off += chunk) take(d.feed(sc.cu8.data() + 2 * off, std::min(chunk, n - off), demod::SampleFormat::kCu8));
    take(d.flush());
    return out;
  };
  const auto whole = run(65536), pieces = run(13);  // 13: every window collected sample by sample
  ASSERT_GT(whole.size(), 100u);
  ASSERT_EQ(whole.size(), pieces.size());
  for (std::size_t i = 0; i < whole.size(); ++i) {
    EXPECT_EQ(hex(whole[i].bytes, whole[i].len), hex(pieces[i].bytes, pieces[i].len));
    EXPECT_EQ(whole[i].sample_pos, pieces[i].sample_pos);
    EXPECT_EQ(whole[i].noise, pieces[i].noise);
  }
}

TEST(DemodSkip, SkippingNothingChangesNothing) {
  const Scenario sc = scenario(30, 0.5, 59);
  demod::Demodulator d(demod::DemodConfig{});
  std::size_t n = 0;
  for (std::size_t off = 0; off < sc.cu8.size() / 2; off += 8192) {
    n += d.skip(0).size();
    n += d.feed(sc.cu8.data() + 2 * off, std::min<std::size_t>(8192, sc.cu8.size() / 2 - off), demod::SampleFormat::kCu8).size();
  }
  n += d.flush().size();
  EXPECT_EQ(n, direct(sc.cu8, 8192).size());
  EXPECT_EQ(d.stats().skipped, 0u);
}

// A gap in the samples: frames before it, a frame cut by it (lost), and frames
// after it keep their positions on the original sample clock.
TEST(DemodSkip, FramesAfterAGapKeepTheirAbsolutePositions) {
  const Scenario sc = scenario(40, 1.0, 51);
  const std::size_t n = sc.cu8.size() / 2;
  const std::size_t gap_at = n / 3, gap = 50000;
  demod::Demodulator d(demod::DemodConfig{});
  std::vector<demod::DemodFrame> out;
  auto take = [&](std::span<const demod::DemodFrame> f) { out.insert(out.end(), f.begin(), f.end()); };
  for (std::size_t off = 0; off < gap_at; off += 65536) take(d.feed(sc.cu8.data() + 2 * off, std::min<std::size_t>(65536, gap_at - off), demod::SampleFormat::kCu8));
  take(d.skip(gap));
  EXPECT_EQ(d.position(), gap_at + gap);
  for (std::size_t off = gap_at + gap; off < n; off += 65536) take(d.feed(sc.cu8.data() + 2 * off, std::min<std::size_t>(65536, n - off), demod::SampleFormat::kCu8));
  take(d.flush());
  EXPECT_EQ(d.stats().skipped, gap);
  // Every frame is either real (in the truth) or one the gap-free stream
  // also gives at the same place (the demodulator's own rare false DF11):
  // the gap creates nothing.
  const auto full = direct(sc.cu8, 65536);
  auto in_full = [&](const demod::DemodFrame& f) {
    for (const auto& g : full) {
      if (g.len == f.len && std::memcmp(g.bytes, f.bytes, f.len) == 0 && std::fabs(g.sample_pos - f.sample_pos) < 0.5) return true;
    }
    return false;
  };
  std::size_t before = 0, after = 0;
  for (const auto& f : out) {
    EXPECT_TRUE(in_truth(sc.plan, f.bytes, f.len, f.sample_pos) || in_full(f)) << f.sample_pos;
    const double end = f.sample_pos + (8.0 + 8.0 * f.len) * 2.4;
    EXPECT_TRUE(end <= static_cast<double>(gap_at) || f.sample_pos >= static_cast<double>(gap_at + gap));
    (f.sample_pos < static_cast<double>(gap_at) ? before : after) += 1;
  }
  EXPECT_GT(before, 50u);
  EXPECT_GT(after, 100u);
  // Nearly everything outside the gap that the full stream decodes.
  std::size_t full_outside = 0;
  for (const auto& f : full) {
    const double end = f.sample_pos + (8.0 + 8.0 * f.len) * 2.4;
    if (end <= static_cast<double>(gap_at) || f.sample_pos >= static_cast<double>(gap_at + gap)) ++full_outside;
  }
  EXPECT_GE(out.size() + 3, full_outside);
}

// ---------------------------------------------------------------------------
// IqSource
// ---------------------------------------------------------------------------

TEST(IqSource, UnpacedGivesExactlyTheDemodulatorsFrames) {
  const Scenario sc = scenario(60, 1.5, 52);
  const std::string path = temp_file("unpaced", sc.cu8);
  for (std::size_t block : {std::size_t{4096}, std::size_t{16384}, std::size_t{65536}}) {
    IqSourceConfig cfg;
    cfg.path = path;
    cfg.speed = 0;
    cfg.block_samples = block;
    cfg.buffers = 3;
    IqSource src(cfg);
    std::string err;
    ASSERT_TRUE(src.open(&err)) << err;
    std::vector<record::Record> recs;
    std::atomic<bool> stop{false};
    src.run(stop, [&](const record::Record& r) { recs.push_back(r); });
    const auto ref = direct(sc.cu8, block);
    ASSERT_EQ(recs.size(), ref.size()) << block;
    std::uint64_t last_rx = 0;
    for (std::size_t i = 0; i < recs.size(); ++i) {
      EXPECT_EQ(hex(recs[i].payload, recs[i].len), hex(ref[i].bytes, ref[i].len));
      EXPECT_EQ(recs[i].type, ref[i].len == 7 ? '2' : '3');
      EXPECT_EQ(recs[i].radio_ts, static_cast<std::uint64_t>(std::llround(ref[i].sample_pos * 5.0)));  // 12 MHz / 2.4 MS/s
      EXPECT_GE(recs[i].rx_ns, last_rx);
      last_rx = recs[i].rx_ns;
    }
    const auto st = src.stats();
    EXPECT_EQ(st.samples, sc.cu8.size() / 2);
    EXPECT_EQ(st.blocks_dropped, 0u);  // unpaced: the reader waits
    EXPECT_EQ(st.frames, recs.size());
    EXPECT_FALSE(st.read_error);
  }
  std::remove(path.c_str());
}

// Paced at up to 2x real time, and at most a third of what this build's
// demodulator manages (debug and sanitizer builds are many times slower than
// release ones): each frame's rx_ns is when its last
// sample "arrived" on the paced clock, so the sink sees it a little later,
// but no later than about a block plus processing.
TEST(IqSource, PacedFrameTimesFollowTheSampleClock) {
  const Scenario sc = scenario(60, 1.0, 53);
  const std::string path = temp_file("paced", sc.cu8);
  const std::uint64_t m0 = mono_ns();
  (void)direct(sc.cu8, 8192);
  const double demod_speed = static_cast<double>(sc.cu8.size() / 2) / 2.4e6 / (static_cast<double>(mono_ns() - m0) * 1e-9);
  const double speed = std::min(2.0, demod_speed / 3);
  IqSourceConfig cfg;
  cfg.path = path;
  cfg.speed = speed;
  cfg.block_samples = 8192;  // 3.4 ms of samples
  IqSource src(cfg);
  ASSERT_TRUE(src.open(nullptr));
  std::vector<std::pair<record::Record, std::uint64_t>> got;
  std::atomic<bool> stop{false};
  const std::uint64_t t0 = mono_ns();
  src.run(stop, [&](const record::Record& r) { got.emplace_back(r, mono_ns()); });
  const double wall = static_cast<double>(mono_ns() - t0) * 1e-9;
  const auto st = src.stats();
  EXPECT_LE(st.blocks_dropped * 20, st.blocks);  // a stalled runner may lose a few
  ASSERT_GT(got.size(), 100u);
  EXPECT_GT(wall, static_cast<double>(sc.cu8.size() / 2) / 2.4e6 / speed * 0.9);  // really paced
  // rx_ns on the paced clock: (samples / fs) / speed apart.
  const double dt_rx = static_cast<double>(got.back().first.rx_ns - got.front().first.rx_ns);
  const double dt_samples = static_cast<double>(got.back().first.radio_ts - got.front().first.radio_ts) / 12e6 / speed * 1e9;
  EXPECT_NEAR(dt_rx, dt_samples, 5e6 + 0.01 * dt_samples);  // within 5 ms (+1%)
  std::size_t early = 0;
  std::vector<std::uint64_t> delay;
  for (const auto& [r, seen] : got) {
    EXPECT_LE(r.rx_ns, seen + 1000);
    delay.push_back(seen - r.rx_ns);
    // Never before the frame's last sample is due on the paced clock (a
    // block is released only once all of it has "arrived").
    const double end_ns = (static_cast<double>(r.radio_ts) / 12e6 + (8.0 + 8.0 * r.len) * 1e-6) / speed * 1e9;
    if (static_cast<double>(seen - t0) < end_ns - 300'000.0) ++early;
  }
  // Typically about a block plus processing; the median, since a loaded
  // machine (parallel tests, sanitizers) stalls the threads now and then.
  std::nth_element(delay.begin(), delay.begin() + static_cast<std::ptrdiff_t>(delay.size() / 2), delay.end());
  EXPECT_LT(delay[delay.size() / 2], 50'000'000u);
  EXPECT_EQ(early, 0u);
  // The envelope's slope is the paced rate, so frame times rarely need
  // raising (only for overlapping replies or a block delivered early).
  EXPECT_LE(st.time_clamped * 50, got.size());
  std::remove(path.c_str());
}

// Unpaced (as fast as possible, waiting): frames are timed on the sample
// clock, so event time is the recording's, not the read speed's.
TEST(IqSource, UnpacedFramesAreTimedOnTheSampleClock) {
  const Scenario sc = scenario(60, 1.5, 57);
  const std::string path = temp_file("clock", sc.cu8);
  IqSourceConfig cfg;
  cfg.path = path;
  cfg.speed = 0;
  IqSource src(cfg);
  ASSERT_TRUE(src.open(nullptr));
  std::vector<record::Record> recs;
  std::atomic<bool> stop{false};
  src.run(stop, [&](const record::Record& r) { recs.push_back(r); });
  ASSERT_GT(recs.size(), 200u);
  std::size_t off = 0;
  for (const auto& r : recs) {
    const double expect = static_cast<double>(r.radio_ts - recs.front().radio_ts) / 12e6 * 1e9;
    const double got = static_cast<double>(r.rx_ns - recs.front().rx_ns);
    // The last sample, not the first: a long frame ends 120 us after its start.
    if (std::fabs(got - expect - (r.len - recs.front().len) * 8.0 * 1000.0) > 2000.0) ++off;
  }
  EXPECT_LE(off, recs.size() / 100);  // only overlapping replies (raised to stay monotonic)
  std::remove(path.c_str());
}

TEST(IqSource, StopEndsASlowlyPacedRun) {
  const Scenario sc = scenario(10, 0.5, 58);
  const std::string path = temp_file("slowpace", sc.cu8);
  IqSourceConfig cfg;
  cfg.path = path;
  cfg.speed = 0.001;  // the first block is due 6.8 s from now
  IqSource src(cfg);
  ASSERT_TRUE(src.open(nullptr));
  std::atomic<bool> stop{false};
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);
  });
  const std::uint64_t t0 = mono_ns();
  src.run(stop, [](const record::Record&) {});
  stopper.join();
  EXPECT_LT(mono_ns() - t0, 600'000'000u);
  std::remove(path.c_str());
}

TEST(IqSource, AnInvalidConfigurationAllocatesNothingAndFailsToOpen) {
  IqSourceConfig cfg;
  cfg.path = "unused";
  cfg.block_samples = std::size_t{1} << 40;  // would be 2 TB of ring
  cfg.buffers = 100000000;
  const std::uint64_t t0 = mono_ns();
  IqSource src(cfg);
  std::string err;
  EXPECT_FALSE(src.open(&err));
  EXPECT_NE(err.find("block_samples"), std::string::npos) << err;
  EXPECT_LT(mono_ns() - t0, 1'000'000'000u);
}

// Paced input and a consumer slower than the samples: whole blocks are
// dropped and counted; what is decoded around the gaps is still right.
TEST(IqSource, ASlowConsumerLosesWholeBlocksAndCountsThem) {
  const Scenario sc = scenario(60, 2.0, 54);
  const std::string path = temp_file("slow", sc.cu8);
  IqSourceConfig cfg;
  cfg.path = path;
  cfg.speed = 20;
  cfg.block_samples = 8192;
  cfg.buffers = 2;
  IqSource src(cfg);
  ASSERT_TRUE(src.open(nullptr));
  std::vector<record::Record> recs;
  std::atomic<bool> stop{false};
  src.run(stop, [&](const record::Record& r) {
    recs.push_back(r);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // far slower than the frame rate at 20x
  });
  const auto st = src.stats();
  EXPECT_GT(st.blocks_dropped, 10u);
  EXPECT_LE(st.samples_dropped, st.blocks_dropped * 8192);  // the last block may be short
  EXPECT_GT(st.samples_dropped, (st.blocks_dropped - 1) * 8192);
  // Every sample was either demodulated or dropped; the demodulator was
  // told of every gap before a delivered block (drops after the last one
  // never show up as a gap).
  EXPECT_EQ(st.demod.samples + st.samples_dropped, st.samples);
  EXPECT_LE(st.demod.skipped, st.samples_dropped);
  EXPECT_GT(st.demod.skipped, 0u);
  EXPECT_EQ(st.samples, sc.cu8.size() / 2);
  EXPECT_LE(st.ring_high_water, 2u);
  ASSERT_GT(recs.size(), 0u);
  // Real frames, but for the demodulator's own rare false DF11 (a wrong bit
  // in the interrogator-code bits).
  std::size_t other = 0;
  for (const auto& r : recs) {
    if (in_truth(sc.plan, r.payload, r.len, static_cast<double>(r.radio_ts) / 5.0)) continue;
    ++other;
    EXPECT_EQ(r.payload[0] >> 3, 11);
  }
  EXPECT_LE(other * 20, recs.size() + 20);
  std::remove(path.c_str());
}

// A pipe delivers odd-sized pieces (a sample split between writes, blocks
// assembled from many reads), and the frames equal the file's.
TEST(IqSource, APipeInOddPiecesGivesTheSameFrames) {
  const Scenario sc = scenario(40, 1.0, 56);
  const std::string fifo = ::testing::TempDir() + "adsb_iq_fifo_" + std::to_string(::getpid());
  std::remove(fifo.c_str());
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  std::thread writer([&] {
    FILE* f = std::fopen(fifo.c_str(), "wb");
    if (!f) return;
    std::setvbuf(f, nullptr, _IONBF, 0);
    std::size_t off = 0, k = 0;
    const std::size_t sizes[] = {1, 4097, 3, 777, 65536, 5, 12345};
    while (off < sc.cu8.size()) {
      const std::size_t n = std::min(sizes[k++ % 7], sc.cu8.size() - off);
      std::fwrite(sc.cu8.data() + off, 1, n, f);
      off += n;
    }
    std::fclose(f);
  });
  IqSourceConfig cfg;
  cfg.path = fifo;
  cfg.speed = 0;
  cfg.block_samples = 4096;
  IqSource src(cfg);
  ASSERT_TRUE(src.open(nullptr));
  std::vector<record::Record> recs;
  std::atomic<bool> stop{false};
  src.run(stop, [&](const record::Record& r) { recs.push_back(r); });
  writer.join();
  std::remove(fifo.c_str());
  const auto ref = direct(sc.cu8, 4096);
  ASSERT_EQ(recs.size(), ref.size());
  for (std::size_t i = 0; i < recs.size(); ++i) {
    EXPECT_EQ(hex(recs[i].payload, recs[i].len), hex(ref[i].bytes, ref[i].len));
    EXPECT_EQ(recs[i].radio_ts, static_cast<std::uint64_t>(std::llround(ref[i].sample_pos * 5.0)));
  }
  EXPECT_EQ(src.stats().samples, sc.cu8.size() / 2);
}

TEST(IqSource, StopEndsARunEarly) {
  const Scenario sc = scenario(20, 2.0, 55);
  const std::string path = temp_file("stop", sc.cu8);
  IqSourceConfig cfg;
  cfg.path = path;
  cfg.speed = 1;  // 2 s at real time
  IqSource src(cfg);
  ASSERT_TRUE(src.open(nullptr));
  std::atomic<bool> stop{false};
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
  });
  const std::uint64_t t0 = mono_ns();
  src.run(stop, [](const record::Record&) {});
  stopper.join();
  EXPECT_LT(mono_ns() - t0, 1'000'000'000u);
  EXPECT_LT(src.stats().samples, sc.cu8.size() / 2);
  std::remove(path.c_str());
}

TEST(IqSource, ReadsSigmfAndRejectsBadConfigurations) {
  const std::string base = ::testing::TempDir() + "adsb_iq_meta_" + std::to_string(::getpid());
  {
    FILE* f = std::fopen((base + ".sigmf-meta").c_str(), "w");
    ASSERT_NE(f, nullptr);
    std::fputs("{\"global\": {\"core:datatype\": \"ci16_le\", \"core:sample_rate\": 12000000.0}}", f);
    std::fclose(f);
  }
  std::string data;
  demod::SampleFormat fmt{};
  double rate = 0;
  std::string err;
  ASSERT_TRUE(ingest::read_sigmf(base + ".sigmf-data", data, fmt, rate, &err)) << err;
  EXPECT_EQ(data, base + ".sigmf-data");
  EXPECT_EQ(fmt, demod::SampleFormat::kCs16);
  EXPECT_EQ(rate, 12e6);
  EXPECT_FALSE(ingest::read_sigmf(base + ".bin", data, fmt, rate, &err));
  EXPECT_FALSE(ingest::read_sigmf(base + "x.sigmf-meta", data, fmt, rate, &err));
  std::remove((base + ".sigmf-meta").c_str());

  auto bad = [](auto edit) {
    IqSourceConfig c;
    c.path = "x";
    edit(c);
    std::string why;
    return !ingest::validate(c, &why) && !why.empty();
  };
  IqSourceConfig ok;
  ok.path = "x";
  EXPECT_TRUE(ingest::validate(ok));
  EXPECT_TRUE(bad([](IqSourceConfig& c) { c.path.clear(); }));
  EXPECT_TRUE(bad([](IqSourceConfig& c) { c.block_samples = 100; }));
  EXPECT_TRUE(bad([](IqSourceConfig& c) { c.buffers = 1; }));
  EXPECT_TRUE(bad([](IqSourceConfig& c) { c.speed = -1; }));
  EXPECT_TRUE(bad([](IqSourceConfig& c) { c.sample_rate_hz = 1e5; }));
  IqSource missing(ok);
  EXPECT_FALSE(missing.open(&err));
}

}  // namespace
