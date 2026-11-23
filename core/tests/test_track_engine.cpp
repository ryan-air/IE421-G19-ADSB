// Track state engine tests: unit cases for each rule, then an end-to-end
// check against the synthetic generator's ground truth.

#include "adsb/state/track_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "adsb/common/rng.hpp"
#include "adsb/modes/encoder.hpp"
#include "adsb/replay/replay.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb;
using namespace adsb::state;
using protocol::TrackDelete;
using protocol::TrackUpdate;

struct CollectSink {
  std::vector<TrackUpdate> updates;
  std::vector<TrackDelete> deletes;
  void on_update(const TrackUpdate& u) { updates.push_back(u); }
  void on_delete(const TrackDelete& d) { deletes.push_back(d); }
};

constexpr std::uint64_t kSec = 1'000'000'000ULL;
constexpr std::uint32_t kIcao = 0xA1B2C3;

template <typename Engine>
void feed(Engine& e, const modes::Frame& f, std::uint64_t rx_ns, CollectSink& sink, std::uint8_t line = 0) {
  FrameMeta meta;
  meta.rx_ns = rx_ns;
  meta.line = line;
  e.on_frame(f.bytes, f.len, meta, sink);
}

TEST(TrackEngine, AddressParityNeedsConfirmedTrack) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  // DF4 before any CRC-verified frame: the address is unverified -> dropped.
  feed(*e, modes::encode_df4(kIcao, 30000), 10 * kSec, sink);
  EXPECT_EQ(e->track_count(), 0u);
  EXPECT_EQ(e->stats().ap_rejected, 1u);
  // DF11 confirms the address; now DF4 is applied.
  feed(*e, modes::encode_df11(kIcao), 11 * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 30000), 12 * kSec, sink);
  EXPECT_EQ(e->track_count(), 1u);
  EXPECT_EQ(e->stats().ap_accepted, 1u);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(sink.updates.back().alt_ft, 30000);
  EXPECT_TRUE(sink.updates.front().update_mask & protocol::update_mask::kNewTrack);
}

TEST(TrackEngine, CorruptedFramesNeverCreateTracks) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  Rng rng(9);
  for (int i = 0; i < 20'000; ++i) {
    modes::Frame f = modes::encode_identification(kIcao, "TEST123 ", 0xA3);
    f.bytes[rng.below(14)] ^= static_cast<std::uint8_t>(1u << rng.below(8));
    feed(*e, f, (100 + static_cast<std::uint64_t>(i)) * 1'000'000ULL, sink);
    modes::Frame g = modes::encode_df5(static_cast<std::uint32_t>(rng.below(0xFFFFFF)), 0x1200);
    feed(*e, g, (100 + static_cast<std::uint64_t>(i)) * 1'000'000ULL, sink);
  }
  EXPECT_EQ(e->track_count(), 0u) << "bit errors and unverified A/P addresses must not create tracks";
  // A flip in the 5 format bits turns DF17 into e.g. DF16/20/21/24, which
  // decode as Address/Parity frames and are rejected as unverified; every
  // other flip fails the CRC. Either way, nothing gets through.
  EXPECT_EQ(e->stats().bad_frames + e->stats().ap_rejected, 40'000u);
  EXPECT_GT(e->stats().bad_frames, 19'000u);
}

// With single-bit repair one wrong bit no longer loses the frame, and it
// still never invents an aircraft: every repaired frame is the real one.
TEST(TrackEngine, SingleBitRepairRecoversFramesWithoutPhantoms) {
  EngineConfig cfg;
  cfg.fix_single_bit = true;
  cfg.dedup_window_ns = 0;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  Rng rng(9);
  int format_flips = 0;
  // A clean frame confirms the aircraft first: repaired frames only update.
  feed(*e, modes::encode_identification(kIcao, "TEST123 ", 0xA3), 50'000'000ULL, sink);
  for (int i = 0; i < 20'000; ++i) {
    modes::Frame f = modes::encode_identification(kIcao, "TEST123 ", 0xA3);
    const auto bit = rng.below(112);
    format_flips += bit < 5;
    f.bytes[bit / 8] ^= static_cast<std::uint8_t>(0x80u >> (bit % 8));
    feed(*e, f, (100 + static_cast<std::uint64_t>(i)) * 1'000'000ULL, sink);
  }
  EXPECT_EQ(e->track_count(), 1u);
  EXPECT_EQ(e->stats().corrected, 20'000u - static_cast<std::uint64_t>(format_flips));
  EXPECT_EQ(e->stats().corrected_rejected, 0u);
  for (const auto& u : sink.updates) EXPECT_EQ(u.icao, kIcao);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(std::string(sink.updates.front().callsign.data(), 8), "TEST123 ");
  // Off (the default), the same frames are all lost.
  auto off = std::make_unique<TrackEngine>();
  modes::Frame f = modes::encode_identification(kIcao, "TEST123 ", 0xA3);
  f.bytes[9] ^= 0x04;
  feed(*off, f, kSec, sink);
  EXPECT_EQ(off->track_count(), 0u);
  EXPECT_EQ(off->stats().corrected, 0u);
}

// Any frame one bit away from a valid one passes a repair, so garbage gets
// through 107 times more often (about 6 in a million random DF17 frames
// here): a repaired frame must never create or confirm a track.
TEST(TrackEngine, RepairedFramesNeverCreateTracks) {
  EngineConfig cfg;
  cfg.fix_single_bit = true;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  Rng rng(21);
  for (int i = 0; i < 1'000'000; ++i) {
    std::uint8_t f[14];
    for (auto& b : f) b = static_cast<std::uint8_t>(rng.below(256));
    f[0] = static_cast<std::uint8_t>(0x88 | (f[0] & 0x07));  // DF17
    FrameMeta meta;
    meta.rx_ns = (100 + static_cast<std::uint64_t>(i)) * 1'000'000ULL;
    e->on_frame(f, 14, meta, sink);
  }
  EXPECT_GT(e->stats().corrected_rejected, 0u);  // some garbage did "repair"
  EXPECT_EQ(e->track_count(), 0u);
  EXPECT_TRUE(sink.updates.empty());
}

// The same transmission clean on one line and repaired on another is a duplicate.
TEST(TrackEngine, ARepairedCopyOfAFrameIsADuplicate) {
  EngineConfig cfg;
  cfg.fix_single_bit = true;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  const modes::Frame clean = modes::encode_identification(kIcao, "TEST123 ", 0xA3);
  feed(*e, clean, kSec, sink, 0);
  modes::Frame bad = clean;
  bad.bytes[9] ^= 0x04;
  feed(*e, bad, kSec + 1000, sink, 1);
  EXPECT_EQ(e->stats().corrected, 1u);
  EXPECT_EQ(e->stats().duplicates, 1u);
}

TEST(TrackEngine, DuplicatesAreDropped) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  const auto f = modes::encode_identification(kIcao, "DUP1    ", 0xA3);
  feed(*e, f, 10 * kSec, sink, 0);
  feed(*e, f, 10 * kSec + 400'000, sink, 1);  // same frame via line B, 0.4 ms later
  EXPECT_EQ(e->stats().duplicates, 1u);
  feed(*e, f, 15 * kSec, sink);  // legitimately repeated 5 s later: not a duplicate
  EXPECT_EQ(e->stats().duplicates, 1u);
}

TEST(TrackEngine, OutOfOrderFieldUpdatesAreIgnored) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 31000), 20 * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 29000), 19 * kSec, sink);  // older: ignored
  // Two field groups in that frame were stale: altitude and air/ground status.
  EXPECT_EQ(e->stats().out_of_order, 2u);
  EXPECT_EQ(sink.updates.back().alt_ft, 31000);
}

TEST(TrackEngine, GlobalThenLocalPositionDecoding) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  const double lat = 40.5, lon = -88.0;
  feed(*e, modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(lat, lon, false, false)), 10 * kSec, sink);
  EXPECT_EQ(e->stats().cpr_unresolved, 1u) << "a single frame cannot be resolved globally";
  feed(*e, modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(lat + 0.001, lon, true, false)),
       10 * kSec + 500'000'000, sink);
  EXPECT_EQ(e->stats().cpr_global, 1u);
  ASSERT_NE(sink.updates.back().lat_e7, TrackUpdate::kLatE7Null);
  EXPECT_NEAR(sink.updates.back().lat_e7 / 1e7, lat + 0.001, 1e-4);
  EXPECT_NEAR(sink.updates.back().lon_e7 / 1e7, lon, 1e-4);

  // 20 s later the pair window has passed: the next frame decodes locally.
  feed(*e, modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(lat + 0.02, lon, false, false)),
       31 * kSec, sink);
  EXPECT_EQ(e->stats().cpr_local, 1u);
  EXPECT_NEAR(sink.updates.back().lat_e7 / 1e7, lat + 0.02, 1e-4);
}

TEST(TrackEngine, ImplausibleJumpIsRejectedThenReanchored) {
  EngineConfig cfg;
  cfg.reanchor_after_rejects = 2;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  auto pos = [&](double la, double lo, bool odd) {
    feed(*e, modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(la, lo, odd, false)), t, sink);
    t += 500'000'000;
  };
  pos(40.0, -88.0, false);
  pos(40.0, -88.0, true);  // established at (40, -88)
  ASSERT_EQ(e->stats().cpr_global, 1u);
  // The aircraft "teleports" 3 degrees north. The first new frame pairs with
  // the old odd frame, cannot be resolved globally, and its local decode is
  // rejected. The next two global fixes are rejected too (reanchor after 2),
  // and the third consecutive disagreeing global fix replaces the position.
  pos(43.0, -88.0, false);
  pos(43.0, -88.0, true);
  pos(43.0, -88.0, false);
  EXPECT_EQ(e->stats().cpr_rejected, 3u);
  EXPECT_EQ(e->stats().cpr_reanchored, 0u);
  pos(43.0, -88.0, true);
  EXPECT_EQ(e->stats().cpr_reanchored, 1u);
  EXPECT_NEAR(sink.updates.back().lat_e7 / 1e7, 43.0, 1e-3);
}

TEST(TrackEngine, ExpiryAfterTimeout) {
  EngineConfig cfg;
  cfg.track_timeout_ns = 60 * kSec;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 100 * kSec + 300'000'000, sink);
  feed(*e, modes::encode_df11(0xA00001), 130 * kSec, sink);
  e->advance(160 * kSec, sink);
  EXPECT_TRUE(sink.deletes.empty()) << "not yet 60 s since the last frame";
  e->advance(161 * kSec + 400'000'000, sink);
  ASSERT_EQ(sink.deletes.size(), 1u) << "expires within one second of its timeout, not 64 s late";
  EXPECT_EQ(sink.deletes[0].icao, kIcao);
  EXPECT_EQ(e->track_count(), 1u);
  // A long gap in the stream (10 minutes) still expires everything.
  e->advance(800 * kSec, sink);
  EXPECT_EQ(sink.deletes.size(), 2u);
  EXPECT_EQ(e->track_count(), 0u);
}

TEST(TrackEngine, OutOfOrderCreatedTrackExpiresPromptly) {
  // Regression: a track created by a frame older than the timeout relative
  // to the wheel was filed in a bucket already passed and expired ~64 s late.
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 100 * kSec, sink);
  e->advance(300 * kSec, sink);
  ASSERT_EQ(sink.deletes.size(), 1u);
  feed(*e, modes::encode_df11(0xA00001), 200 * kSec, sink);  // arrives late
  e->advance(301 * kSec + 1, sink);
  EXPECT_EQ(sink.deletes.size(), 2u) << "already past its timeout: expires on the next tick";
}

TEST(TrackEngine, RadioTimestampCarriesLineId) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  FrameMeta m;
  m.rx_ns = 10 * kSec;
  m.radio_ts = 0x123456789ABCULL;  // 48-bit Beast counter
  m.line = 1;                      // receiver line B
  const auto f = modes::encode_identification(kIcao, "LINEB   ", 0xA3);
  e->on_frame(f.bytes, f.len, m, sink);
  ASSERT_EQ(sink.updates.size(), 1u);
  EXPECT_EQ(sink.updates[0].radio_ts >> 60, 1u) << "top 4 bits: line id";
  EXPECT_EQ(sink.updates[0].radio_ts & 0x0FFFFFFFFFFFFFFFULL, 0x123456789ABCULL);
}

TEST(TrackEngine, DeleteCarriesTheExpiryTime) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 100 * kSec + 250'000'000, sink);
  e->advance(1000 * kSec, sink);  // checked long after the fact
  ASSERT_EQ(sink.deletes.size(), 1u);
  EXPECT_EQ(sink.deletes[0].event_ts_ns, 160 * kSec + 250'000'000)
      << "last frame + timeout, independent of when advance() ran";
}

TEST(TrackEngine, RefreshedTrackDoesNotExpire) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  for (std::uint64_t s = 0; s < 600; s += 20) feed(*e, modes::encode_df11(kIcao), (1000 + s) * kSec, sink);
  EXPECT_TRUE(sink.deletes.empty());
  EXPECT_EQ(e->track_count(), 1u);
}

// ---------------------------------------------------------------------------
// End to end against ground truth
// ---------------------------------------------------------------------------

struct EndToEnd : ::testing::Test {
  static sim::SynthConfig config() {
    sim::SynthConfig c;
    c.seed = 2024;
    c.aircraft = 60;
    c.duration_s = 90;
    c.lines = 2;
    c.corrupt_rate = 0.01;  // well above real-world rates
    return c;
  }
};

TEST_F(EndToEnd, TracksMatchGroundTruth) {
  const auto cfg = config();
  const sim::SynthOutput s = sim::generate(cfg);
  EngineConfig ecfg;
  ecfg.has_receiver = true;
  ecfg.receiver_lat = cfg.receiver_lat;
  ecfg.receiver_lon = cfg.receiver_lon;
  auto e = std::make_unique<TrackEngine>(ecfg);
  CollectSink sink;
  for (const auto& r : s.records) {
    FrameMeta m;
    m.rx_ns = r.rx_ns;
    m.radio_ts = r.radio_ts;
    m.line = r.line;
    e->on_frame(r.payload, r.len, m, sink);
  }

  // Index truth by aircraft.
  std::map<std::uint32_t, std::vector<const sim::TruthPoint*>> truth;
  for (const auto& t : s.truth) truth[t.icao].push_back(&t);
  std::map<std::uint32_t, const sim::AircraftInfo*> info;
  for (const auto& a : s.aircraft) info[a.icao] = &a;

  // 1. No phantom aircraft, despite 1% corrupted frames.
  std::set<std::uint32_t> tracked;
  for (const auto& u : sink.updates) tracked.insert(u.icao);
  for (const auto icao : tracked) ASSERT_TRUE(info.count(icao)) << std::hex << icao;
  EXPECT_GE(tracked.size(), static_cast<std::size_t>(cfg.aircraft * 0.9)) << "most aircraft are in range";

  // 2. Every reported position is within CPR resolution of the true position
  //    at the time the frame was sent.
  int checked = 0;
  double worst_m = 0;
  for (const auto& u : sink.updates) {
    if (!(u.update_mask & protocol::update_mask::kPosition)) continue;
    const auto& pts = truth[u.icao];
    // The frame was sent at most a few ms before it was received, and
    // positions are >= 0.4 s apart: the latest transmission <= rx is it.
    const auto it = std::upper_bound(pts.begin(), pts.end(), u.event_ts_ns,
                                     [](std::uint64_t t, const sim::TruthPoint* p) { return t < p->tx_ns; });
    ASSERT_NE(it, pts.begin());
    const sim::TruthPoint& tp = **(it - 1);
    const double dlat = (u.lat_e7 / 1e7 - tp.lat) * 111'320.0;
    const double dlon = std::remainder(u.lon_e7 / 1e7 - tp.lon, 360.0) * 111'320.0 * std::cos(tp.lat * std::numbers::pi / 180);
    const double err_m = std::hypot(dlat, dlon);
    worst_m = std::max(worst_m, err_m);
    ASSERT_LT(err_m, tp.on_ground ? 5.0 : 12.0) << std::hex << u.icao;
    ++checked;
  }
  EXPECT_GT(checked, 5000);
  RecordProperty("worst_position_error_m", std::to_string(worst_m));

  // 3. Callsigns, squawks, and categories match the simulated aircraft.
  for (const auto& u : sink.updates) {
    const auto* a = info[u.icao];
    if (u.update_mask & protocol::update_mask::kCallsign) {
      ASSERT_EQ(std::string(u.callsign.data(), 8), std::string(a->callsign, 8));
    }
    if (u.squawk != TrackUpdate::kSquawkNull) {
      ASSERT_EQ(u.squawk, a->squawk);
    }
    if (u.category != 0) {
      ASSERT_EQ(u.category, a->category);
    }
    if (u.flags & protocol::track_flags::kOnGround) {
      ASSERT_TRUE(a->on_ground);
    }
  }

  // 3b. Ground speed and track agree with the truth for airborne aircraft.
  //     Velocity frames are sent up to ~0.6 s from the nearest position
  //     frame, so allow for turns (<= 3 deg/s) plus 1 kt quantization.
  int vel_checked = 0;
  for (const auto& u : sink.updates) {
    if (!(u.update_mask & protocol::update_mask::kTrack) || u.gs_dkt == TrackUpdate::kGsDktNull) continue;
    const auto& pts = truth[u.icao];
    const auto it = std::upper_bound(pts.begin(), pts.end(), u.event_ts_ns,
                                     [](std::uint64_t t, const sim::TruthPoint* p) { return t < p->tx_ns; });
    if (it == pts.begin() || (*(it - 1))->on_ground) continue;
    const sim::TruthPoint& tp = **(it - 1);
    ASSERT_NEAR(u.gs_dkt / 10.0, tp.ground_speed_kt, 1.5) << std::hex << u.icao;
    ASSERT_NEAR(std::remainder(u.track_cdeg / 100.0 - tp.track_deg, 360.0), 0.0, 3.0) << std::hex << u.icao;
    ++vel_checked;
  }
  EXPECT_GT(vel_checked, 1000);

  // 4. The two receiver lines produce duplicates, which are filtered.
  EXPECT_GT(e->stats().duplicates, 1000u);
  EXPECT_EQ(e->stats().cpr_rejected, 0u) << "clean traffic never trips the plausibility check";

  // 5. Flushing expires every track.
  e->advance(s.records.back().rx_ns + 120 * kSec, sink);
  EXPECT_EQ(e->track_count(), 0u);
  EXPECT_EQ(sink.deletes.size(), tracked.size());
}

TEST_F(EndToEnd, NaiveEngineProducesIdenticalOutput) {
  // The naive baseline must be a pure data-structure swap: same messages,
  // byte for byte, in the same order.
  const auto s = sim::generate(config());
  auto run = [&](auto& engine) {
    replay::HashingSink sink;
    for (const auto& r : s.records) {
      FrameMeta m;
      m.rx_ns = r.rx_ns;
      m.radio_ts = r.radio_ts;
      m.line = r.line;
      engine.on_frame(r.payload, r.len, m, sink);
    }
    engine.advance(s.records.back().rx_ns + 120 * kSec, sink);
    return sink.hex_digest();
  };
  auto flat = std::make_unique<TrackEngine>();
  auto naive = std::make_unique<NaiveTrackEngine>();
  EXPECT_EQ(run(*flat), run(*naive));
}

}  // namespace
