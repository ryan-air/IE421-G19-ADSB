// A/B line arbitration (adsb/ingest/arbiter.hpp, README_UPDATES.md §6).

#include "adsb/ingest/arbiter.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <tuple>
#include <vector>

#include "adsb/common/build_config.hpp"
#include "adsb/common/rng.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/replay/replay.hpp"
#include "adsb/sim/synth.hpp"

namespace adsb {
namespace {

using ingest::Arbiter;
using ingest::ArbiterConfig;
using record::Record;

Record frame(std::uint64_t rx_ns, std::uint8_t line, std::uint8_t id, std::uint8_t len = 14,
             std::uint64_t radio_ts = 0) {
  Record r;
  r.rx_ns = rx_ns;
  r.line = line;
  r.len = len;
  r.type = len == 14 ? '3' : '2';
  r.radio_ts = radio_ts;
  r.payload[0] = 0x8D;
  r.payload[1] = id;
  r.payload[len - 1] = static_cast<std::uint8_t>(id * 7 + 3);
  return r;
}

constexpr std::uint64_t kT0 = 1'000'000'000'000ULL;
constexpr std::uint64_t kUs = 1000;

TEST(Arbiter, ForwardsTheFirstCopyAndDropsTheOtherLines) {
  Arbiter a;
  EXPECT_TRUE(a.offer(frame(kT0, 0, 1)));
  EXPECT_FALSE(a.offer(frame(kT0 + 100 * kUs, 1, 1)));
  EXPECT_TRUE(a.offer(frame(kT0 + 200 * kUs, 1, 2)));   // B first this time
  EXPECT_FALSE(a.offer(frame(kT0 + 230 * kUs, 0, 2)));
  EXPECT_TRUE(a.offer(frame(kT0 + 300 * kUs, 1, 3)));   // only B
  a.finish();
  const auto& s = a.stats();
  EXPECT_EQ(s.offered, 5u);
  EXPECT_EQ(s.forwarded, 3u);
  EXPECT_EQ(s.dropped, 2u);
  EXPECT_EQ(s.multi, 2u);
  EXPECT_EQ(s.line[0].received, 2u);
  EXPECT_EQ(s.line[0].first, 1u);
  EXPECT_EQ(s.line[0].wins, 1u);
  EXPECT_EQ(s.line[0].late, 1u);
  EXPECT_EQ(s.line[0].only, 0u);
  EXPECT_EQ(s.line[1].first, 2u);
  EXPECT_EQ(s.line[1].wins, 1u);
  EXPECT_EQ(s.line[1].late, 1u);
  EXPECT_EQ(s.line[1].only, 1u);
  EXPECT_EQ(a.margin(0).count(), 1u);
  EXPECT_NEAR(static_cast<double>(a.margin(0).max()), 100e3, 100e3 * 0.002);
  EXPECT_NEAR(static_cast<double>(a.margin(1).max()), 30e3, 30e3 * 0.002);
  EXPECT_EQ(a.open_entries(), 0u);
}

TEST(Arbiter, TheWindowIsInclusiveAndARepeatAfterItIsForwarded) {
  ArbiterConfig cfg;
  cfg.window_ns = 5'000'000;
  Arbiter a(cfg);
  EXPECT_TRUE(a.offer(frame(kT0, 0, 9)));
  EXPECT_FALSE(a.offer(frame(kT0 + cfg.window_ns, 1, 9)));       // exactly at the edge: same frame
  EXPECT_TRUE(a.offer(frame(kT0 + cfg.window_ns + 1, 1, 9)));    // the first copy has left the window
  // A real repeat (identical payload ~1 s later, like a DF11 squitter).
  EXPECT_TRUE(a.offer(frame(kT0 + 1'000'000'000ULL, 0, 9)));
  a.finish();
  EXPECT_EQ(a.stats().forwarded, 3u);
  EXPECT_EQ(a.stats().multi, 1u);
}

TEST(Arbiter, ASameLineRepeatIsDroppedButIsNotAMultiLineFrame) {
  Arbiter a;
  EXPECT_TRUE(a.offer(frame(kT0, 0, 4)));
  EXPECT_FALSE(a.offer(frame(kT0 + 10 * kUs, 0, 4)));  // demodulator double detection
  a.finish();
  const auto& s = a.stats();
  EXPECT_EQ(s.line[0].same_line, 1u);
  EXPECT_EQ(s.line[0].late, 0u);
  EXPECT_EQ(s.multi, 0u);
  EXPECT_EQ(s.line[0].only, 1u);
  EXPECT_EQ(a.margin(0).count(), 0u);
}

TEST(Arbiter, ThreeLinesCountTheFrameOnceAndRecordEachLateCopy) {
  Arbiter a;
  EXPECT_TRUE(a.offer(frame(kT0, 2, 5)));
  EXPECT_FALSE(a.offer(frame(kT0 + 50 * kUs, 0, 5)));
  EXPECT_FALSE(a.offer(frame(kT0 + 80 * kUs, 1, 5)));
  EXPECT_FALSE(a.offer(frame(kT0 + 90 * kUs, 1, 5)));  // line 1 again: same-line
  a.finish();
  const auto& s = a.stats();
  EXPECT_EQ(s.multi, 1u);
  EXPECT_EQ(s.line[2].wins, 1u);
  EXPECT_EQ(s.line[0].late, 1u);
  EXPECT_EQ(s.line[1].late, 1u);
  EXPECT_EQ(s.line[1].same_line, 1u);
  EXPECT_EQ(a.margin(2).count(), 2u);
  // Line 1's copy matched line 2's first copy: no 0-vs-1 clock sample from it,
  // but line 0's did not pair with line 1 either (neither was first).
  EXPECT_EQ(s.clock01.n, 0u);
}

TEST(Arbiter, ShortAndLongFramesWithTheSamePrefixAreDifferent) {
  Arbiter a;
  Record s = frame(kT0, 0, 1, 7);
  Record l = frame(kT0, 1, 1, 14);
  std::memset(l.payload, 0, sizeof l.payload);
  std::memcpy(l.payload, s.payload, 7);
  EXPECT_TRUE(a.offer(s));
  EXPECT_TRUE(a.offer(l));
}

TEST(Arbiter, LinesOutsideTheTableModeACAndBadLengthsPassThrough) {
  Arbiter a;
  EXPECT_TRUE(a.offer(frame(kT0, 7, 1)));
  EXPECT_TRUE(a.offer(frame(kT0, 7, 1)));
  Record bad = frame(kT0, 0, 2);
  bad.len = 0;
  EXPECT_TRUE(a.offer(bad));
  // Mode A/C: two bytes every aircraft on the same code shares.
  Record ac = frame(kT0, 0, 3);
  ac.len = 2;
  ac.type = '1';
  EXPECT_TRUE(a.offer(ac));
  ac.line = 1;
  EXPECT_TRUE(a.offer(ac));
  EXPECT_EQ(a.stats().unarbitrated, 5u);
  EXPECT_EQ(a.stats().forwarded, 5u);
  EXPECT_EQ(a.open_entries(), 0u);
}

// The index holds ring positions: an entry counter passing 2^32 (an hour at
// 1 M frames/s) must not alias the index's empty marker.
TEST(Arbiter, KeepsWorkingAcrossThe32BitEntryCounter) {
  ArbiterConfig cfg;
  cfg.max_entries = 16;
  cfg.first_sequence = 0xFFFFFFF0ULL;
  Arbiter a(cfg);
  for (int round = 0; round < 4; ++round) {
    const std::uint64_t t = kT0 + static_cast<std::uint64_t>(round) * std::uint64_t{1'000'000'000};
    for (std::uint8_t i = 0; i < 12; ++i) EXPECT_TRUE(a.offer(frame(t + i, 0, i))) << round << " " << int(i);
    for (std::uint8_t i = 0; i < 12; ++i) EXPECT_FALSE(a.offer(frame(t + 20 + i, 1, i))) << round << " " << int(i);
  }
  EXPECT_EQ(a.next_sequence(), 0xFFFFFFF0ULL + 48);  // the counter did pass 2^32
  a.finish();
  EXPECT_EQ(a.stats().multi, 48u);
}

TEST(Arbiter, TimeGoingBackwardsIsRaisedToTheLatestTime) {
  Arbiter a;
  EXPECT_TRUE(a.offer(frame(kT0 + 10'000 * kUs, 0, 1)));
  EXPECT_TRUE(a.offer(frame(kT0, 1, 2)));             // 10 ms earlier than the latest
  EXPECT_FALSE(a.offer(frame(kT0 + 10'001 * kUs, 0, 2)));  // still inside its (raised) window
  EXPECT_EQ(a.stats().time_backwards, 1u);
}

TEST(Arbiter, AFullTableEvictsTheOldestEarlyAndCountsIt) {
  ArbiterConfig cfg;
  cfg.max_entries = 16;
  Arbiter a(cfg);
  for (std::uint8_t i = 0; i < 17; ++i) EXPECT_TRUE(a.offer(frame(kT0 + i, 0, i)));
  EXPECT_EQ(a.stats().evicted_early, 1u);
  EXPECT_EQ(a.open_entries(), 16u);
  // The evicted frame's copy is no longer recognised (the documented cost)...
  EXPECT_TRUE(a.offer(frame(kT0 + 20, 1, 0)));
  // ... the others still are.
  for (std::uint8_t i = 2; i < 17; ++i) EXPECT_FALSE(a.offer(frame(kT0 + 30, 1, i))) << int(i);
}

// A brute-force model: every first copy is remembered with its time; a frame
// is dropped iff an earlier first copy with the same bytes is within the
// window. Random traffic over a small payload alphabet makes index clusters,
// wrap-around and backward-shift deletions happen constantly.
TEST(Arbiter, AgreesWithABruteForceModelOnRandomTraffic) {
  for (const std::size_t entries : {std::size_t{16}, std::size_t{64}, std::size_t{1024}}) {
    ArbiterConfig cfg;
    cfg.window_ns = 50 * kUs;
    cfg.max_entries = entries;
    Arbiter a(cfg);
    struct First {
      std::uint64_t t;
      std::uint8_t len;
      std::uint8_t id;
      std::uint8_t seen;
      std::uint8_t first_line;
    };
    std::vector<First> model;  // open windows, oldest first
    std::uint64_t only[ingest::kMaxArbLines] = {}, multi = 0, late = 0, same = 0;
    auto retire = [&](const First& f) {
      if (std::popcount(static_cast<unsigned>(f.seen)) == 1) ++only[f.first_line];
    };
    Rng rng(entries);
    std::uint64_t t = kT0;
    const std::size_t cap = std::bit_ceil(entries);
    for (int n = 0; n < 200'000; ++n) {
      t += rng.below(4 * kUs);
      const auto line = static_cast<std::uint8_t>(rng.below(3));
      const auto id = static_cast<std::uint8_t>(rng.below(48));
      const std::uint8_t len = rng.chance(0.3) ? 7 : 14;
      while (!model.empty() && t - model.front().t > cfg.window_ns) {
        retire(model.front());
        model.erase(model.begin());
      }
      auto it = std::find_if(model.begin(), model.end(),
                             [&](const First& f) { return f.id == id && f.len == len; });
      bool expect_forward = it == model.end();
      if (!expect_forward) {
        const auto bit = static_cast<std::uint8_t>(1u << line);
        if (it->seen & bit) {
          ++same;
        } else {
          if (it->seen == (1u << it->first_line)) ++multi;
          it->seen = static_cast<std::uint8_t>(it->seen | bit);
          ++late;
        }
      } else {
        if (model.size() == cap) {
          retire(model.front());
          model.erase(model.begin());
        }
        model.push_back({t, len, id, static_cast<std::uint8_t>(1u << line), line});
      }
      ASSERT_EQ(a.offer(frame(t, line, id, len)), expect_forward) << "op " << n << " entries " << entries;
    }
    a.finish();
    for (const auto& f : model) retire(f);
    const auto& s = a.stats();
    EXPECT_EQ(s.multi, multi);
    std::uint64_t got_late = 0, got_same = 0;
    for (int l = 0; l < 3; ++l) {
      EXPECT_EQ(s.line[l].only, only[l]) << l;
      got_late += s.line[l].late;
      got_same += s.line[l].same_line;
    }
    EXPECT_EQ(got_late, late);
    EXPECT_EQ(got_same, same);
    EXPECT_GT(multi, 1000u);
    if (entries == 16) {
      EXPECT_GT(s.evicted_early, 0u);
    }
  }
}

TEST(Arbiter, FitsTheRelativeRadioClockAcrossA48BitWrap) {
  // Line 1 runs 25 ppm fast and starts 5,000 ticks ahead; line 0 wraps its
  // 48-bit counter halfway through.
  Arbiter a;
  constexpr std::uint64_t kMask = (std::uint64_t{1} << 48) - 1;
  const std::uint64_t start0 = kMask - 6'000'000;  // wraps after 0.5 s of 12 MHz
  for (int i = 0; i < 2000; ++i) {
    const double t_s = i * 0.0005;  // 1 s
    const auto ticks0 = static_cast<std::uint64_t>(std::llround(t_s * 12e6));
    const auto ticks1 = static_cast<std::uint64_t>(std::llround(t_s * 12e6 * (1 + 25e-6))) + 5000;
    const std::uint64_t rx = kT0 + static_cast<std::uint64_t>(t_s * 1e9);
    const Record r0 = frame(rx, 0, static_cast<std::uint8_t>(i), 14, (start0 + ticks0) & kMask);
    const Record r1 = frame(rx, 1, static_cast<std::uint8_t>(i), 14, (start0 + ticks1) & kMask);
    // Line 0 first on even frames, line 1 first on odd ones: both orders pair.
    const Record& first = i % 2 == 0 ? r0 : r1;
    Record second = i % 2 == 0 ? r1 : r0;
    second.rx_ns += 300 * kUs;
    EXPECT_TRUE(a.offer(first));
    EXPECT_FALSE(a.offer(second));
  }
  EXPECT_EQ(a.stats().clock01.n, 2000u);
  EXPECT_NEAR(a.clock_ppm(), 25.0, 0.01);
  EXPECT_NEAR(a.stats().clock01.offset_at(0.0), 5000.0, 1.0);
  EXPECT_LT(a.stats().clock01.residual_rms(), 1.0);
  const std::string rep = ingest::arbiter_report(a);
  EXPECT_NE(rep.find("arb clock   line 1 vs 0: rate +25.0"), std::string::npos) << rep;
  EXPECT_NE(rep.find("arb line 0  received 2000, first 1000, wins 1000, late 1000"), std::string::npos) << rep;
}

// Interrogation replies can repeat byte for byte inside the window: a line-0
// copy of one reply can pair with a line-1 copy of another. Only DF17/18
// pairs feed the clock fit, and pairs far off the fitted line are rejected.
TEST(Arbiter, TheClockFitIgnoresRepliesAndRejectsMismatchedPairs) {
  Arbiter a;
  constexpr double kPpm = 18.0;
  auto ts = [](double t_s, double ppm, std::uint64_t off) {
    return off + static_cast<std::uint64_t>(std::llround(t_s * 12e6 * (1 + ppm * 1e-6)));
  };
  for (int i = 0; i < 4000; ++i) {
    const double t_s = i * 0.001;
    const std::uint64_t rx = kT0 + static_cast<std::uint64_t>(t_s * 1e9);
    const auto id = static_cast<std::uint8_t>(i);
    if (i % 50 == 7) {
      // A mismatched DF17 pair: line 1's copy has a timestamp 20 ms or
      // 0.3 ms (3,600 ticks; the threshold is 1,200) off.
      const double off_s = i % 100 == 7 ? 0.02 : 0.0003;
      EXPECT_TRUE(a.offer(frame(rx, 0, id, 14, ts(t_s, 0, 1000))));
      EXPECT_FALSE(a.offer(frame(rx + 10, 1, id, 14, ts(t_s + off_s, kPpm, 9000))));
    } else if (i % 50 == 9) {
      // A DF4 reply pair, also mismatched: never used.
      Record r0 = frame(rx, 0, id, 7, ts(t_s, 0, 1000));
      r0.payload[0] = 0x20;
      Record r1 = r0;
      r1.line = 1;
      r1.radio_ts = ts(t_s + 0.02, kPpm, 9000);
      EXPECT_TRUE(a.offer(r0));
      EXPECT_FALSE(a.offer(r1));
    } else {
      EXPECT_TRUE(a.offer(frame(rx, 0, id, 14, ts(t_s, 0, 1000))));
      EXPECT_FALSE(a.offer(frame(rx + 10, 1, id, 14, ts(t_s, kPpm, 9000))));
    }
  }
  EXPECT_NEAR(a.clock_ppm(), kPpm, 0.01);
  EXPECT_NEAR(a.stats().clock01.offset_at(0.0), 8000.0, 1.0);
  EXPECT_LT(a.stats().clock01.residual_rms(), 1.0);
  EXPECT_EQ(a.stats().clock_rejected, 80u);  // every mismatched DF17 pair
  EXPECT_EQ(a.stats().clock01.n, 4000u - 80u - 80u);  // the DF4 pairs are not offered to the fit
  EXPECT_EQ(a.stats().clock_restarts, 0u);
}

TEST(Arbiter, TheClockFitHandlesAnOffsetNearHalfTheCounterAndARestart) {
  // Line 1 is 2^47 - 100 ticks ahead and 30 ppm fast (+360 ticks/s), so the
  // difference crosses the signed range after ~0.3 s.
  Arbiter a;
  constexpr std::uint64_t kMask = (std::uint64_t{1} << 48) - 1;
  const std::uint64_t off = (std::uint64_t{1} << 47) - 100;
  auto add = [&](int i, std::uint64_t off1) {
    const double t_s = i * 0.001;
    const std::uint64_t rx = kT0 + static_cast<std::uint64_t>(t_s * 1e9);
    const auto t0 = 5000 + static_cast<std::uint64_t>(std::llround(t_s * 12e6));
    const auto t1 = 5000 + off1 + static_cast<std::uint64_t>(std::llround(t_s * 12e6 * (1 + 30e-6)));
    EXPECT_TRUE(a.offer(frame(rx, 0, static_cast<std::uint8_t>(i), 14, t0 & kMask)));
    EXPECT_FALSE(a.offer(frame(rx + 10, 1, static_cast<std::uint8_t>(i), 14, t1 & kMask)));
  };
  for (int i = 0; i < 1000; ++i) add(i, off);
  EXPECT_NEAR(a.clock_ppm(), 30.0, 0.05);
  EXPECT_LT(a.stats().clock01.residual_rms(), 1.0);
  EXPECT_EQ(a.stats().clock_rejected, 0u);
  // Line 1's receiver restarts: its counter jumps. After 64 rejections the
  // fit starts over and finds the rate again.
  for (int i = 1000; i < 3000; ++i) add(i, 777'777'777);
  EXPECT_EQ(a.stats().clock_restarts, 1u);
  EXPECT_EQ(a.stats().clock_rejected, 64u);
  EXPECT_NEAR(a.clock_ppm(), 30.0, 0.05);
}

// ---- Against the generator's ground truth -----------------------------------

// Each record traced back to its transmission: the same bytes, or one bit
// flipped (the generator's corruption), transmitted at most 3 ms earlier.
struct Traced {
  std::size_t tx = 0;   // index into transmissions
  bool intact = false;
};

std::vector<Traced> trace(const sim::SynthOutput& out) {
  std::vector<Traced> t(out.records.size());
  for (std::size_t i = 0; i < out.records.size(); ++i) {
    const Record& r = out.records[i];
    const auto lo = std::lower_bound(out.transmissions.begin(), out.transmissions.end(), r.rx_ns - 3'000'000,
                                     [](const sim::Transmission& x, std::uint64_t v) { return x.tx_ns < v; });
    bool found = false;
    for (auto it = lo; it != out.transmissions.end() && it->tx_ns <= r.rx_ns; ++it) {
      if (it->len != r.len) continue;
      int bits = 0;
      for (int b = 0; b < r.len; ++b) bits += std::popcount(static_cast<unsigned>(it->bytes[b] ^ r.payload[b]));
      if (bits <= 1) {
        t[i] = {static_cast<std::size_t>(it - out.transmissions.begin()), bits == 0};
        found = true;
        if (bits == 0) break;
      }
    }
    EXPECT_TRUE(found) << "record " << i;
  }
  return t;
}

sim::SynthConfig ab_config() {
  sim::SynthConfig cfg;
  cfg.seed = 11;
  cfg.aircraft = 200;
  cfg.duration_s = 40;
  cfg.lines = 2;
  cfg.corrupt_rate = 0.01;
  cfg.keep_transmissions = true;
  return cfg;
}

TEST(ArbiterTruth, ForwardsEveryReceivedFrameOnceAndTheEarliestCopy) {
  const sim::SynthOutput out = sim::generate(ab_config());
  const auto tr = trace(out);
  // Truth: group records by (transmission, received bytes).
  struct Group {
    std::uint8_t lines = 0;
    std::size_t earliest = 0;  // record index
    int forwarded = 0;
  };
  // Key: (transmission, received bytes packed into two words, length).
  std::map<std::tuple<std::size_t, std::uint64_t, std::uint64_t>, Group> groups;
  auto group_of = [&](std::size_t i) -> Group& {
    const Record& r = out.records[i];
    std::uint64_t w[2] = {0, 0};
    std::memcpy(w, r.payload, r.len);
    return groups[{tr[i].tx, w[0], w[1] ^ (std::uint64_t{r.len} << 56)}];
  };
  for (std::size_t i = 0; i < out.records.size(); ++i) {
    Group& g = group_of(i);
    if (g.lines == 0) g.earliest = i;  // records are in rx order
    g.lines = static_cast<std::uint8_t>(g.lines | (1u << out.records[i].line));
  }
  Arbiter a;
  for (std::size_t i = 0; i < out.records.size(); ++i) {
    if (a.offer(out.records[i])) {
      Group& g = group_of(i);
      ++g.forwarded;
      EXPECT_EQ(g.earliest, i) << "a later copy was forwarded";
    }
  }
  a.finish();
  std::uint64_t only0 = 0, only1 = 0, both = 0, wins0 = 0;
  for (const auto& [k, g] : groups) {
    EXPECT_EQ(g.forwarded, 1);
    if (g.lines == 1) ++only0;
    if (g.lines == 2) ++only1;
    if (g.lines == 3) {
      ++both;
      if (out.records[g.earliest].line == 0) ++wins0;
    }
  }
  const auto& s = a.stats();
  EXPECT_EQ(s.forwarded, groups.size());
  EXPECT_EQ(s.line[0].only, only0);
  EXPECT_EQ(s.line[1].only, only1);
  EXPECT_EQ(s.multi, both);
  EXPECT_EQ(s.line[0].wins, wins0);
  EXPECT_EQ(s.line[1].wins, both - wins0);
  EXPECT_EQ(s.line[0].same_line + s.line[1].same_line, 0u);
  EXPECT_EQ(s.evicted_early, 0u);
  // The generator's line B is 250 us slower plus 0-300 us jitter on each line:
  // line A wins most frames, by ~250 us.
  EXPECT_GT(wins0, both * 8 / 10);
  EXPECT_NEAR(static_cast<double>(a.margin(0).percentile(50)), 250e3, 30e3);
  // Relative radio clock: the generator's line B drifts +18 ppm.
  EXPECT_NEAR(a.clock_ppm(), 18.0, 0.01);
  EXPECT_LT(a.stats().clock01.residual_rms(), 1.0);  // ticks: rounding only
  // The union is larger than either line alone.
  EXPECT_GT(s.forwarded, s.line[0].received);
  EXPECT_GT(s.forwarded, s.line[1].received);
}

bool same(const Record& a, const Record& b) {
  return a.rx_ns == b.rx_ns && a.radio_ts == b.radio_ts && a.line == b.line && a.type == b.type &&
         a.signal == b.signal && a.len == b.len && std::memcmp(a.payload, b.payload, a.len) == 0;
}

TEST(ArbiterTruth, AnOutageChangesNothingButThatLinesFramesInsideIt) {
  sim::SynthConfig cfg = ab_config();
  const sim::SynthOutput base = sim::generate(cfg);
  cfg.outage[1] = {10.0, 25.0};
  const sim::SynthOutput cut = sim::generate(cfg);
  std::vector<Record> expect;
  std::uint64_t removed = 0;
  for (const Record& r : base.records) {
    const double t_rx = static_cast<double>(r.rx_ns - cfg.start_rx_ns) * 1e-9;
    // Transmit time is within a few ms of rx; skip the edges.
    if (r.line == 1 && t_rx > 10.01 && t_rx < 24.99) {
      ++removed;
      continue;
    }
    if (r.line == 1 && (std::abs(t_rx - 10.0) < 0.01 || std::abs(t_rx - 25.0) < 0.01)) continue;
    expect.push_back(r);
  }
  EXPECT_GT(removed, 10'000u);
  EXPECT_EQ(cut.stats.outage, base.records.size() - cut.records.size());
  // Every record outside the outage is unchanged.
  std::size_t j = 0;
  for (const Record& r : expect) {
    while (j < cut.records.size() && cut.records[j].rx_ns < r.rx_ns) ++j;
    bool hit = false;
    for (std::size_t k = j; k < cut.records.size() && cut.records[k].rx_ns == r.rx_ns; ++k) {
      if (same(cut.records[k], r)) hit = true;
    }
    ASSERT_TRUE(hit) << "rx " << r.rx_ns;
  }
}

TEST(ArbiterTruth, WithEachLineOutInTurnTheOutputHasNoGap) {
  sim::SynthConfig cfg = ab_config();
  cfg.outage[0] = {5.0, 15.0};
  cfg.outage[1] = {20.0, 30.0};
  const sim::SynthOutput out = sim::generate(cfg);
  const auto tr = trace(out);
  // Every intact transmission any line received is forwarded intact.
  std::vector<char> received(out.transmissions.size(), 0), forwarded(out.transmissions.size(), 0);
  Arbiter a;
  for (std::size_t i = 0; i < out.records.size(); ++i) {
    if (tr[i].intact) received[tr[i].tx] = 1;
    if (a.offer(out.records[i]) && tr[i].intact) {
      EXPECT_EQ(forwarded[tr[i].tx], 0);
      forwarded[tr[i].tx] = 1;
    }
  }
  std::size_t mismatched = 0, got = 0;
  for (std::size_t i = 0; i < received.size(); ++i) {
    mismatched += received[i] != forwarded[i] ? 1u : 0u;
    got += received[i] ? 1u : 0u;
  }
  EXPECT_EQ(mismatched, 0u);
  EXPECT_GT(got, 10'000u);
  // And the track state never loses an aircraft: with a 60 s timeout nothing
  // could expire anyway, so check the gap directly: the longest silence of a
  // forwarded airborne aircraft is no longer than in the no-outage run.
  auto longest_gap = [](const sim::SynthOutput& o, Arbiter& arb) {
    std::map<std::uint32_t, std::uint64_t> last;
    std::uint64_t worst = 0;
    for (const Record& r : o.records) {
      if (!arb.offer(r) || r.len != 14) continue;
      const std::uint32_t icao = (std::uint32_t{r.payload[1]} << 16) | (std::uint32_t{r.payload[2]} << 8) | r.payload[3];
      auto [it, fresh] = last.try_emplace(icao, r.rx_ns);
      if (!fresh) {
        worst = std::max(worst, r.rx_ns - it->second);
        it->second = r.rx_ns;
      }
    }
    return worst;
  };
  // Each line alone (the other one never works), no outages.
  sim::SynthConfig only_a = cfg, only_b = cfg;
  only_a.outage[0] = {};
  only_a.outage[1] = {0.0, 1e9};
  only_b.outage[0] = {0.0, 1e9};
  only_b.outage[1] = {};
  Arbiter x, y, z;
  const std::uint64_t gap_ab = longest_gap(out, x);
  const std::uint64_t gap_a = longest_gap(sim::generate(only_a), y);
  const std::uint64_t gap_b = longest_gap(sim::generate(only_b), z);
  // At every moment one line works, so no silence is longer than one line's
  // own longest silence; an outage would add ~10 s.
  EXPECT_LE(gap_ab, std::max(gap_a, gap_b));
  EXPECT_LT(gap_ab, 5'000'000'000ULL);
}

// ---- Through replay --------------------------------------------------------

namespace fs = std::filesystem;

class ArbiterReplay : public ::testing::Test {
 protected:
  void SetUp() override {
    sim::SynthConfig cfg = ab_config();
    cfg.keep_transmissions = false;
    const auto out = sim::generate(cfg);
    path_ = (fs::temp_directory_path() / ("adsb_arb_" + std::to_string(::getpid()) + ".adsbrec")).string();
    record::RecordingWriter w;
    std::string err;
    ASSERT_TRUE(w.open(path_, sim::recording_info(cfg), &err)) << err;
    for (const auto& r : out.records) ASSERT_TRUE(w.append(r));
    ASSERT_TRUE(w.close());
  }
  void TearDown() override { fs::remove(path_); }
  std::string path_;
};

TEST_F(ArbiterReplay, InlineAndPipelineMakeTheSameDecisionsAndOutput) {
  record::RecordingReader rd;
  std::string err;
  ASSERT_TRUE(rd.open(path_, &err)) << err;
  auto run = [&](bool pipeline, std::string& report) {
    Arbiter arb;
    replay::ReplayOptions opt;
    opt.flush_at_end = true;
    opt.arbiter = &arb;
    auto eng = std::make_unique<DefaultTrackEngine>(state::EngineConfig{});
    const auto res = pipeline ? replay::replay_pipeline(rd, *eng, opt, nullptr) : replay::replay_inline(rd, *eng, opt);
    report = ingest::arbiter_report(arb);
    EXPECT_EQ(arb.open_entries(), 0u);
    EXPECT_EQ(res.stats.duplicates, 0u) << "the engine saw a cross-line duplicate";
    return res.hash;
  };
  std::string r1, r2;
  const std::string h1 = run(false, r1), h2 = run(true, r2);
  EXPECT_EQ(h1, h2);
  EXPECT_EQ(r1, r2);
  EXPECT_NE(r1.find("arb line 1"), std::string::npos) << r1;
}

}  // namespace
}  // namespace adsb
