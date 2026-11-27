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
#include <tuple>
#include <vector>

#include "adsb/common/rng.hpp"
#include "adsb/modes/encoder.hpp"
#include "adsb/replay/replay.hpp"
#include "adsb/sim/synth.hpp"
#include "test_util.hpp"

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
  // The altitude was stale. (Flight status 0 says nothing about air/ground:
  // only "on ground" is taken from Address/Parity replies.)
  EXPECT_EQ(e->stats().out_of_order, 1u);
  EXPECT_EQ(sink.updates.back().alt_ft, 31000);
}

// ---- Altitude plausibility (readsb's rules) ---------------------------------

std::int32_t last_alt(const CollectSink& s) {
  for (auto it = s.updates.rbegin(); it != s.updates.rend(); ++it) {
    if (it->update_mask & protocol::update_mask::kAltitude) return it->alt_ft;
  }
  return TrackUpdate::kAltFtNull;
}

bool ever_shown(const CollectSink& s, std::int32_t alt) {
  return std::any_of(s.updates.begin(), s.updates.end(), [&](const TrackUpdate& u) {
    return (u.update_mask & protocol::update_mask::kAltitude) && u.alt_ft == alt;
  });
}

// The real case: an ADS-B aircraft at FL313, then one DF0 that claims
// 4,900 ft a second later. A lower source is ignored while ADS-B is fresh.
TEST(TrackEngine, AddressParityAltitudeIsIgnoredWhileAdsbIsFresh) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_airborne_position(kIcao, 31300, modes::cpr_encode(41.9, -87.6, false, false)), 10 * kSec,
       sink);
  feed(*e, modes::encode_df4(kIcao, 4900), 11 * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 31200), 24 * kSec, sink);  // 14 s: still inside the window
  EXPECT_EQ(e->stats().altitude_lower_source, 2u);
  EXPECT_FALSE(ever_shown(sink, 4900));
  EXPECT_EQ(last_alt(sink), 31300);
  // 16 s after the ADS-B altitude a Mode S altitude is considered, and one
  // that agrees is taken.
  feed(*e, modes::encode_df4(kIcao, 31200), 26 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 31200);
}

// Mode S only: replies that agree build the score; one that implies
// 100,000+ ft/min is rejected and the altitude stays.
TEST(TrackEngine, OneImplausibleReplyCannotMoveAModeSAircraft) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  for (std::uint64_t i = 0; i < 5; ++i) feed(*e, modes::encode_df4(kIcao, 31000), (2 + i) * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 4900), 7 * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 31000), 8 * kSec, sink);
  EXPECT_EQ(e->stats().altitude_implausible, 1u);
  EXPECT_EQ(e->stats().altitude_dropped, 0u);
  EXPECT_FALSE(ever_shown(sink, 4900));
  EXPECT_EQ(last_alt(sink), 31000);
}

// With a score of 1 a rejection empties it: the altitude is cleared (as
// readsb invalidates it) and the next altitude is taken as new.
TEST(TrackEngine, ARejectionThatEmptiesTheScoreClearsTheAltitude) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 31000), 2 * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 4900), 3 * kSec, sink);
  EXPECT_EQ(e->stats().altitude_dropped, 1u);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(sink.updates.back().alt_ft, TrackUpdate::kAltFtNull);
  EXPECT_TRUE(sink.updates.back().update_mask & protocol::update_mask::kAltitude);
  feed(*e, modes::encode_df4(kIcao, 4900), 4 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 4900);
}

// A change of 300 ft or more must fit a climb rate: +-12,500 ft/min without
// a vertical rate, the vertical rate +- 1,500 ft/min (plus half its age)
// with one.
TEST(TrackEngine, AltitudeChangesMustFitTheClimbRate) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  // Event times start at 1,000 s, so a vertical rate without its own
  // timestamp would look older than 60 s.
  const auto feed = [&](const modes::Frame& f, std::uint64_t t, CollectSink& s) {
    ::feed(*e, f, 1000 * kSec + t, s);
  };
  feed(modes::encode_df11(kIcao), 1 * kSec, sink);
  for (std::uint64_t i = 0; i < 5; ++i) feed(modes::encode_df4(kIcao, 10000), (2 + i) * kSec, sink);
  // +1,000 ft in 3 s = 15,000 ft/min: outside 12,500.
  feed(modes::encode_df4(kIcao, 11000), 9 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 10000);
  EXPECT_EQ(e->stats().altitude_implausible, 1u);
  // +500 ft in 3 s = 7,500 ft/min: within 12,500.
  feed(modes::encode_df4(kIcao, 10500), 9 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 10500);
  // Descending at 2,000 ft/min: +500 ft in 3 s is outside (-5,000, 1,000).
  feed(modes::encode_velocity(kIcao, 300, 0, -2000), 9 * kSec, sink);
  feed(modes::encode_df4(kIcao, 11000), 12 * kSec, sink);
  EXPECT_EQ(e->stats().altitude_implausible, 2u);
  EXPECT_EQ(last_alt(sink), 10500);
  // -250 ft is under the 300 ft threshold: always taken.
  feed(modes::encode_df4(kIcao, 10250), 13 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 10250);
}

// ADS-B is trusted: a large change arriving quickly is taken (readsb
// prefers that to missing a real emergency descent).
TEST(TrackEngine, AdsbAltitudeChangesAreTrusted) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  auto pos = [&](std::int32_t alt, std::uint64_t t) {
    feed(*e, modes::encode_airborne_position(kIcao, alt, modes::cpr_encode(41.9, -87.6, false, false)), t, sink);
  };
  pos(31000, 10 * kSec);
  pos(31000, 11 * kSec);
  pos(20000, 12 * kSec);
  EXPECT_EQ(last_alt(sink), 20000);
  EXPECT_EQ(e->stats().altitude_implausible, 0u);
}

// The real SWA700 case: parked at Midway (surface positions), then a DF0
// with VS 0 and 27,000 ft, then one with VS 1 and 325 ft. The aircraft
// stays on the ground; its first altitude cannot be checked against
// anything (readsb takes it too), and the second, implausible against it,
// clears it.
TEST(TrackEngine, AParkedAircraftStaysOnTheGroundThroughAMisattributedReply) {
  EngineConfig cfg;
  cfg.has_receiver = true;
  cfg.receiver_lat = 41.878;
  cfg.receiver_lon = -87.647;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  const std::uint32_t icao = 0xA5AA78;
  for (std::uint64_t i = 0; i < 3; ++i) {
    feed(*e, modes::encode_surface_position(icao, 5, 90, modes::cpr_encode(41.7860, -87.7520, i % 2 == 1, true)),
         (5 + i) * kSec, sink);
  }
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_TRUE(sink.updates.back().flags & protocol::track_flags::kOnGround);
  const auto a = adsb::test::hex("02E19190CA27F5");
  const auto b = adsb::test::hex("064600B5024FD1");
  FrameMeta meta;
  meta.rx_ns = 25 * kSec;
  e->on_frame(a.data(), a.size(), meta, sink);
  EXPECT_TRUE(sink.updates.back().flags & protocol::track_flags::kOnGround);
  meta.rx_ns = 31 * kSec;
  e->on_frame(b.data(), b.size(), meta, sink);
  EXPECT_TRUE(sink.updates.back().flags & protocol::track_flags::kOnGround);
  EXPECT_EQ(e->stats().altitude_dropped, 1u);
}

// The score fades over 30 s without an altitude: after a long silence any
// altitude is taken (it cannot be checked against a stale one).
TEST(TrackEngine, TheScoreFadesWithTheAltitudesAge) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  for (std::uint64_t i = 0; i < 5; ++i) feed(*e, modes::encode_df4(kIcao, 31000), (2 + i) * kSec, sink);
  feed(*e, modes::encode_df11(kIcao), 30 * kSec, sink);  // keeps the track alive
  feed(*e, modes::encode_df4(kIcao, 4900), 40 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 4900);
  EXPECT_EQ(e->stats().altitude_implausible, 0u);
}

// An altitude from a better source than the stored one is taken even when
// it is trusted less than the score (ADS-B arriving slowly: a third).
TEST(TrackEngine, ABetterSourceReplacesAModeSAltitude) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  for (std::uint64_t i = 0; i < 10; ++i) feed(*e, modes::encode_df4(kIcao, 31000), 2 * kSec + i * 500'000'000ULL, sink);
  feed(*e, modes::encode_airborne_position(kIcao, 25000, modes::cpr_encode(41.9, -87.6, false, false)), 9 * kSec,
       sink);
  EXPECT_EQ(last_alt(sink), 25000);
}

// TIS-B (DF18 CF 2) ranks below ADS-B: ignored while ADS-B is fresh.
TEST(TrackEngine, TisBAltitudeIsIgnoredWhileAdsbIsFresh) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_airborne_position(kIcao, 31300, modes::cpr_encode(41.9, -87.6, false, false)), 10 * kSec,
       sink);
  auto tisb = modes::encode_airborne_position(kIcao, 31200, modes::cpr_encode(41.9, -87.6, true, false));
  tisb.bytes[0] = static_cast<std::uint8_t>((18u << 3) | 2u);  // DF18, CF 2 (TIS-B, ICAO address)
  modes::write_parity(tisb.bytes, tisb.len, 0);
  feed(*e, tisb, 11 * kSec, sink);
  EXPECT_EQ(e->stats().altitude_lower_source, 1u);
  EXPECT_EQ(last_alt(sink), 31300);
  // DF18 CF 0 (ADS-B from a non-transponder device) also ranks below DF17.
  tisb.bytes[0] = static_cast<std::uint8_t>(18u << 3);
  modes::write_parity(tisb.bytes, tisb.len, 0);
  feed(*e, tisb, 12 * kSec, sink);
  EXPECT_EQ(e->stats().altitude_lower_source, 2u);
}

// The score is capped by the altitude's age: after 15 s of silence a score
// of 15 counts as 10, so ten implausible replies clear the altitude.
TEST(TrackEngine, TheScoreIsCappedByTheAltitudesAge) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  for (std::uint64_t i = 0; i < 15; ++i) feed(*e, modes::encode_df4(kIcao, 31000), 2 * kSec + i * 100'000'000ULL, sink);
  const std::uint64_t t0 = 2 * kSec + 14 * 100'000'000ULL + 15 * kSec;
  feed(*e, modes::encode_df11(kIcao), t0 - kSec, sink);
  for (std::uint64_t i = 0; i < 9; ++i) feed(*e, modes::encode_df4(kIcao, 4900), t0 + i * 10'000'000ULL, sink);
  EXPECT_EQ(e->stats().altitude_dropped, 0u);
  feed(*e, modes::encode_df4(kIcao, 4900), t0 + 9 * 10'000'000ULL, sink);
  EXPECT_EQ(e->stats().altitude_dropped, 1u);
}

// The vertical rate's margin grows with its age (half the age in ms): a
// 21 s old descent rate no longer rules out a climb of 7,500 ft/min.
TEST(TrackEngine, AnOldVerticalRateWidensTheBand) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  feed(*e, modes::encode_velocity(kIcao, 300, 0, -2000), 2 * kSec, sink);
  for (std::uint64_t i = 3; i <= 20; ++i) feed(*e, modes::encode_df4(kIcao, 10000), i * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 10500), 23 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 10500);
  EXPECT_EQ(e->stats().altitude_implausible, 0u);
}

// A vertical rate counts for 60 s: an expired descent rate no longer
// narrows the band (+725 ft in 3 s = 10,875 ft/min is outside the
// 71 s old rate's band of (-14,500, 10,500) but inside +-12,500).
TEST(TrackEngine, AVerticalRateExpiresAfterSixtySeconds) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 1 * kSec, sink);
  feed(*e, modes::encode_velocity(kIcao, 300, 0, -2000), 2 * kSec, sink);
  for (std::uint64_t i = 3; i <= 70; ++i) feed(*e, modes::encode_df4(kIcao, 10000), i * kSec, sink);
  feed(*e, modes::encode_df4(kIcao, 10725), 73 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 10725);
}

// When a rejection clears a GNSS altitude, its GNSS flag goes too.
TEST(TrackEngine, ClearingAGnssAltitudeClearsItsFlag) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_airborne_position(kIcao, 31000, modes::cpr_encode(41.9, -87.6, false, false), 20), 10 * kSec,
       sink);
  ASSERT_TRUE(sink.updates.back().flags & protocol::track_flags::kAltitudeGnss);
  feed(*e, modes::encode_df11(kIcao), 30 * kSec, sink);
  // 29.5 s later the score is down to 1; one implausible reply empties it.
  feed(*e, modes::encode_df4(kIcao, 4900), 39 * kSec + 500'000'000ULL, sink);
  EXPECT_EQ(e->stats().altitude_dropped, 1u);
  EXPECT_EQ(sink.updates.back().alt_ft, TrackUpdate::kAltFtNull);
  EXPECT_FALSE(sink.updates.back().flags & protocol::track_flags::kAltitudeGnss);
}

// ---- Air/ground (readsb's rules) --------------------------------------------

bool on_ground(const CollectSink& s) {
  return !s.updates.empty() && (s.updates.back().flags & protocol::track_flags::kOnGround) != 0;
}

// A transponder with CA 7 never says "airborne": the first airborne
// position after surface positions clears "on ground".
TEST(TrackEngine, AnAirbornePositionEndsTheGroundStateOfACa7Transponder) {
  EngineConfig cfg;
  cfg.has_receiver = true;
  cfg.receiver_lat = 41.878;
  cfg.receiver_lon = -87.647;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  for (std::uint64_t i = 0; i < 3; ++i) {
    feed(*e, modes::encode_surface_position(kIcao, 20, 90, modes::cpr_encode(41.786, -87.752, i % 2 == 1, true), 7, 7),
         (5 + i) * kSec, sink);
  }
  EXPECT_TRUE(on_ground(sink));
  feed(*e, modes::encode_airborne_position(kIcao, 1500, modes::cpr_encode(41.79, -87.76, false, false), 11, 7),
       60 * kSec, sink);
  EXPECT_FALSE(on_ground(sink));
}

// The airborne state an airborne position sets counts as CRC-checked: a
// reply's "on ground" cannot undo it for 120 s, even once the positions are
// no longer recent.
TEST(TrackEngine, AnAirbornePositionOutranksLaterRepliesForTwoMinutes) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao, 7), 1 * kSec, sink);
  feed(*e, modes::encode_df5(kIcao, 0x1200, 1), 2 * kSec, sink);
  EXPECT_TRUE(on_ground(sink));
  feed(*e, modes::encode_airborne_position(kIcao, 1500, modes::cpr_encode(41.79, -87.76, false, false), 11, 7),
       60 * kSec, sink);
  EXPECT_FALSE(on_ground(sink));
  feed(*e, modes::encode_df5(kIcao, 0x1200, 1), 100 * kSec, sink);
  EXPECT_FALSE(on_ground(sink));
  for (std::uint64_t t = 120; t <= 180; t += 20) feed(*e, modes::encode_df11(kIcao, 7), t * kSec, sink);  // keep it alive
  feed(*e, modes::encode_df5(kIcao, 0x1200, 1), 181 * kSec, sink);
  EXPECT_TRUE(on_ground(sink));
}

// A transponder that says "on ground" (CA 4) while sending airborne
// positions stays on the ground: an airborne position only overrides the
// frame's own ground indication right after a surface position.
TEST(TrackEngine, AirbornePositionsWithCa4KeepTheGroundState) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  for (std::uint64_t i = 0; i < 3; ++i) {
    feed(*e, modes::encode_airborne_position(kIcao, 600, modes::cpr_encode(41.786, -87.752, i % 2 == 1, false), 11, 4),
         (10 + i) * kSec, sink);
  }
  EXPECT_TRUE(on_ground(sink));
}

// A reply's "on ground" (FS 1) is ignored while positions are recent, so a
// misattributed reply cannot ground a flying aircraft; without recent
// positions it is taken.
TEST(TrackEngine, AReplysOnGroundIsIgnoredWhilePositionsAreRecent) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_airborne_position(kIcao, 31000, modes::cpr_encode(41.9, -87.6, false, false)), 10 * kSec,
       sink);
  feed(*e, modes::encode_airborne_position(kIcao, 31000, modes::cpr_encode(41.9, -87.6, true, false)),
       10 * kSec + 500'000'000ULL, sink);
  EXPECT_FALSE(on_ground(sink));
  feed(*e, modes::encode_df5(kIcao, 0x1200, 1), 12 * kSec, sink);
  EXPECT_FALSE(on_ground(sink));
  feed(*e, modes::encode_df11(kIcao, 6), 31 * kSec, sink);  // CA 6: alive, no air/ground
  // Positions are old now, but "airborne" came from DF17 (CA 5) 22 s ago:
  // a reply cannot override that for 120 s.
  feed(*e, modes::encode_df5(kIcao, 0x1200, 1), 32 * kSec, sink);
  EXPECT_FALSE(on_ground(sink));
  for (std::uint64_t t = 40; t <= 120; t += 20) feed(*e, modes::encode_df11(kIcao, 6), t * kSec, sink);
  feed(*e, modes::encode_df5(kIcao, 0x1200, 1), 131 * kSec, sink);
  EXPECT_TRUE(on_ground(sink));
}

// A Mode S-only aircraft (no positions; DF11 with CA 6 says nothing about
// air/ground) takes "on ground" from its replies.
TEST(TrackEngine, AModeSOnlyAircraftTakesOnGroundFromReplies) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao, 6), 1 * kSec, sink);
  feed(*e, modes::encode_df5(kIcao, 0x1200, 1), 2 * kSec, sink);
  EXPECT_TRUE(on_ground(sink));
}

// A repaired frame (fix_single_bit) is not trusted like a clean one: it
// cannot move an ADS-B altitude that is reliable.
TEST(TrackEngine, ARepairedFrameCannotJumpAReliableAltitude) {
  EngineConfig cfg;
  cfg.fix_single_bit = true;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  for (std::uint64_t i = 0; i < 3; ++i) {
    feed(*e, modes::encode_airborne_position(kIcao, 31000, modes::cpr_encode(41.9, -87.6, i % 2 == 1, false)),
         10 * kSec + i * 500'000'000ULL, sink);
  }
  auto bad = modes::encode_airborne_position(kIcao, 4900, modes::cpr_encode(41.9, -87.6, true, false));
  bad.bytes[9] ^= 0x10;  // one wrong bit in the ME field: repaired, so the altitude stays 4,900
  feed(*e, bad, 11 * kSec + 500'000'000ULL, sink);
  EXPECT_EQ(e->stats().corrected, 1u);
  EXPECT_EQ(e->stats().altitude_implausible, 1u);
  EXPECT_EQ(last_alt(sink), 31000);
}

TEST(TrackEngine, AltitudePlausibilityCanBeSwitchedOff) {
  EngineConfig cfg;
  cfg.altitude_plausibility = false;
  auto e = std::make_unique<TrackEngine>(cfg);
  CollectSink sink;
  feed(*e, modes::encode_airborne_position(kIcao, 31300, modes::cpr_encode(41.9, -87.6, false, false)), 10 * kSec,
       sink);
  feed(*e, modes::encode_df4(kIcao, 4900), 11 * kSec, sink);
  EXPECT_EQ(last_alt(sink), 4900);
  EXPECT_EQ(e->stats().altitude_lower_source + e->stats().altitude_implausible, 0u);
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
  // The aircraft "teleports" 3 degrees north. Each new frame pairs with an
  // old one (rejected frames are not kept), so every fix disagrees with the
  // stored position: the third rejection in a row (reanchor after 2) clears
  // both CPR slots.
  pos(43.0, -88.0, false);
  pos(43.0, -88.0, true);
  pos(43.0, -88.0, false);
  EXPECT_EQ(e->stats().cpr_rejected, 3u);
  EXPECT_EQ(e->stats().cpr_reanchored, 0u);
  // The next frame has no partner and may not decode against the doubted
  // position; the one after it completes a pair of new frames, and that
  // global fix replaces the position without the jump check.
  pos(43.0, -88.0, true);
  EXPECT_EQ(e->stats().cpr_reanchored, 0u);
  EXPECT_NEAR(sink.updates.back().lat_e7 / 1e7, 40.0, 1e-3);
  pos(43.0, -88.0, false);
  EXPECT_EQ(e->stats().cpr_reanchored, 1u);
  EXPECT_EQ(e->stats().cpr_rejected, 3u);
  EXPECT_NEAR(sink.updates.back().lat_e7 / 1e7, 43.0, 1e-3);
  // Afterwards the track is checked again as usual.
  pos(40.0, -88.0, true);
  EXPECT_EQ(e->stats().cpr_rejected, 4u);
  EXPECT_NEAR(sink.updates.back().lat_e7 / 1e7, 43.0, 1e-3);
}

// The reset forgets both CPR frames: the fix that re-anchors the track is
// never made with a frame from before the reset, which is unchecked.
TEST(TrackEngine, AfterTheResetOnlyNewFramesPair) {
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
  pos(40.0, -88.0, true);
  ASSERT_EQ(e->stats().cpr_global, 1u);
  // Three even frames 18 NM east (same longitude zones, so a pair with the
  // stored odd frame resolves): rejected three times, then both slots are
  // cleared.
  pos(40.0, -87.6, false);
  pos(40.0, -87.6, false);
  pos(40.0, -87.6, false);
  ASSERT_EQ(e->stats().cpr_rejected, 3u);
  // Another even frame: the old odd frame is gone, so there is no pair (and
  // no local decode against the doubted position).
  pos(40.0, -87.6, false);
  EXPECT_EQ(e->stats().cpr_global, 1u);
  EXPECT_EQ(e->stats().cpr_reanchored, 0u);
  EXPECT_NEAR(sink.updates.back().lon_e7 / 1e7, -88.0, 1e-3);
  pos(40.0, -87.6, true);
  EXPECT_EQ(e->stats().cpr_reanchored, 1u);
  EXPECT_NEAR(sink.updates.back().lon_e7 / 1e7, -87.6, 1e-3);
}

// Real frames (professor's capture, 2026-11-19, receiver near Chicago): one
// CRC-valid frame in each sequence carries CPR values of another place (a
// frame wrongly repaired upstream, most likely). Kept in its slot, it paired
// with each later frame of the other format, and the third rejection in a
// row used to re-anchor the track there: A12AA1 showed in Antarctica for 5 s,
// A35254 near (-1.6, 0.3). A rejected frame is now dropped, so every
// position stays on the real track.
struct TimedHex {
  std::uint64_t ms;
  const char* hex;
};

void expect_no_phantom(const std::vector<TimedHex>& frames, double lat, double lon, std::uint64_t min_fixes) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  for (const auto& f : frames) {
    const auto b = adsb::test::hex(f.hex);
    FrameMeta meta;
    meta.rx_ns = 100 * kSec + f.ms * 1'000'000ULL;
    e->on_frame(b.data(), b.size(), meta, sink);
  }
  std::uint64_t positions = 0;
  for (const auto& u : sink.updates) {
    if (!(u.update_mask & protocol::update_mask::kPosition)) continue;
    ++positions;
    EXPECT_NEAR(u.lat_e7 / 1e7, lat, 0.05) << "update at seq " << positions;
    EXPECT_NEAR(u.lon_e7 / 1e7, lon, 0.05) << "update at seq " << positions;
  }
  EXPECT_GE(e->stats().cpr_global, min_fixes);
  EXPECT_GE(e->stats().cpr_rejected, 1u) << "the corrupt frame's fix is rejected";
  EXPECT_EQ(e->stats().cpr_reanchored, 0u);
  EXPECT_GE(positions, min_fixes);
}

TEST(TrackEngine, OneCorruptOddFrameDoesNotMoveTheTrack) {
  expect_no_phantom({{0, "8DA12AA158A5871CC63C835EEEB6"},
                     {919, "8DA12AA158A5871CD83C5FC5DC0A"},
                     {2852, "8DA12AA158A58392A3BD710CF5DB"},
                     {5984, "8DA12AA158A5871D3A3B9A016012"},
                     {7047, "8DA12AA158A5871D4E3B71DE2D13"},
                     {10593, "8DA12AA158A583933BBC3D732C87"},
                     {12102, "8DA12AA158A584612AB1E57E222A"},  // odd, CPR of another place
                     {13265, "8DA12AA158A583936FBBD33CF110"},
                     {15037, "8DA12AA158A5839391BB8C7AADA8"},  // was the phantom fix
                     {16979, "8DA12AA158A5871E0E39EECC8FE4"},
                     {18815, "8DA12AA158A5871E3039A6151F5F"},
                     {19820, "8DA12AA158A58393EFBACE27A4B6"}},
                    41.36, -89.05, 9);
}

TEST(TrackEngine, OneCorruptEvenFrameDoesNotMoveTheTrack) {
  expect_no_phantom({{0, "8DA3525458A7B36D7D98E1A66031"},
                     {840, "8DA3525458A7B6F85818311189A4"},
                     {3024, "8DA3525458A7936D17986A8529BE"},
                     {3521, "8DA3525458A796F80017CCE99A35"},
                     {4113, "8DA3525458A796F7EC17B4E4D7A3"},
                     {4633, "8DA3525458A7836CE1982A49C62D"},
                     {5073, "8DA3525458A786F7CA178EBCB3F3"},
                     {6921, "8DA3525458A766F78E1748DCC261"},
                     {7889, "8DA3525458A766F76A171D32BC34"},
                     {8481, "8DA3525458A756F7561705A21295"},
                     {9033, "8DA3525458A756F74416F290876B"},
                     {10642, "8DA3525458A742F71016B4CF4AB8"},  // even, CPR of another place
                     {12090, "8DA3525458A736F6DC1679B9EC73"},
                     {13098, "8DA3525458A726F6BE1656A30D92"},  // was the phantom fix
                     {15018, "8DA3525458A7136B7F968D08461A"},
                     {17147, "8DA3525458A706F63215B3993D5C"},
                     {17731, "8DA3525458A5F36B1F961D0CE51D"},
                     {19347, "8DA3525458A5E6F5E8155DE459F9"}},
                    41.13, -89.63, 15);
}

// Review scenarios (synthetic). Receiver near Chicago.
EngineConfig chicago_receiver() {
  EngineConfig cfg;
  cfg.has_receiver = true;
  cfg.receiver_lat = 41.878;
  cfg.receiver_lon = -87.647;
  return cfg;
}

double last_lat(const CollectSink& s) {
  for (auto it = s.updates.rbegin(); it != s.updates.rend(); ++it)
    if (it->update_mask & protocol::update_mask::kPosition) return it->lat_e7 / 1e7;
  return 0.0;
}

double last_lon(const CollectSink& s) {
  for (auto it = s.updates.rbegin(); it != s.updates.rend(); ++it)
    if (it->update_mask & protocol::update_mask::kPosition) return it->lon_e7 / 1e7;
  return 0.0;
}

// Landing 64 NM from the receiver (Milwaukee): a surface frame decoded
// against the receiver lands 1.5 degrees off. The track's own position or a
// pair places the frames, so no correct fix is rejected (a first version of
// this change rejected every one of them and never placed the aircraft on
// the ground).
TEST(TrackEngine, SurfacePositionsFarFromTheReceiverFollowTheTrack) {
  auto e = std::make_unique<TrackEngine>(chicago_receiver());
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  for (std::uint64_t i = 0; i < 4; ++i, t += kSec / 2)
    feed(*e, modes::encode_airborne_position(kIcao, 1000, modes::cpr_encode(42.95, -87.90, i % 2 == 1, false)), t, sink);
  for (std::uint64_t i = 0; i < 60; ++i, t += kSec / 2)
    feed(*e, modes::encode_surface_position(kIcao, 20, 90, modes::cpr_encode(42.947, -87.897, i % 2 == 1, true)), t, sink);
  EXPECT_EQ(e->stats().cpr_rejected, 0u);
  EXPECT_GE(e->stats().cpr_global, 60u);
  EXPECT_NEAR(last_lat(sink), 42.947, 1e-3);
  EXPECT_TRUE(sink.updates.back().flags & protocol::track_flags::kOnGround);
}

// On landing the first surface frame has no surface partner yet; it decodes
// against the track's last (airborne) position, as readsb does.
TEST(TrackEngine, TheFirstSurfaceFrameAfterLandingDecodesAgainstTheTrack) {
  auto e = std::make_unique<TrackEngine>();  // no receiver position
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  for (std::uint64_t i = 0; i < 2; ++i, t += kSec / 2)
    feed(*e, modes::encode_airborne_position(kIcao, 700, modes::cpr_encode(41.786, -87.752, i % 2 == 1, false)), t, sink);
  ASSERT_EQ(e->stats().cpr_global, 1u);
  feed(*e, modes::encode_surface_position(kIcao, 40, 90, modes::cpr_encode(41.7855, -87.7510, true, true)), t, sink);
  EXPECT_EQ(e->stats().cpr_local, 1u);
  EXPECT_NEAR(last_lat(sink), 41.7855, 1e-3);
  EXPECT_NEAR(last_lon(sink), -87.7510, 1e-3);
  EXPECT_TRUE(sink.updates.back().flags & protocol::track_flags::kSurfacePosition);
}

// A surface track whose frames come more than a minute apart: too old to
// decode against the track. Near the receiver (O'Hare, 15 NM) the receiver
// places the frame; at an airport 64 NM away that decode is a zone off and
// is ignored, but the frame is kept, so the next frame pairs with it.
TEST(TrackEngine, SparseSurfaceFramesNearAndFarFromTheReceiver) {
  for (const auto& [lat, lon, near] : {std::tuple{41.9868, -87.9029, true}, std::tuple{42.947, -87.897, false}}) {
    auto e = std::make_unique<TrackEngine>(chicago_receiver());
    CollectSink sink;
    std::uint64_t t = 10 * kSec;
    // 20 frames: a track 107 km out first lands on the aliased place and is
    // re-anchored (ASurfaceTrackStartedAtTheWrongPlaceRecovers).
    for (std::uint64_t i = 0; i < 20; ++i, t += kSec / 2)
      feed(*e, modes::encode_surface_position(kIcao, 5, 90, modes::cpr_encode(lat, lon, i % 2 == 1, true)), t, sink);
    ASSERT_GE(e->stats().cpr_global, 2u) << near;
    ASSERT_NEAR(last_lat(sink), lat, 1e-3) << near;
    // 90 s without positions (past local_decode_max_age_ns); all-call
    // replies keep the track alive (track_timeout_ns 60 s).
    for (int k = 0; k < 9; ++k) {
      t += 10 * kSec;
      feed(*e, modes::encode_df11(kIcao, 4), t, sink);
    }
    const std::uint64_t global = e->stats().cpr_global;
    const std::uint64_t local = e->stats().cpr_local;
    const std::uint64_t rejected = e->stats().cpr_rejected;
    ASSERT_EQ(e->stats().tracks_expired, 0u) << near;
    feed(*e, modes::encode_surface_position(kIcao, 5, 90, modes::cpr_encode(lat + 0.0005, lon, true, true)), t, sink);
    EXPECT_EQ(e->stats().cpr_local, local + (near ? 1u : 0u)) << near;
    EXPECT_EQ(e->stats().cpr_rejected, rejected) << near;
    t += kSec / 2;
    feed(*e, modes::encode_surface_position(kIcao, 5, 90, modes::cpr_encode(lat + 0.0005, lon, false, true)), t, sink);
    EXPECT_EQ(e->stats().cpr_global, global + 1u) << "pairs with the frame before: " << near;
    EXPECT_NEAR(last_lat(sink), lat + 0.0005, 1e-3) << near;
  }
}

// A surface-only track far from the receiver starts at the aliased place
// (its first fix is against the receiver, unchecked); the pairs disagree,
// and the reset re-anchors it on the real airport.
TEST(TrackEngine, ASurfaceTrackStartedAtTheWrongPlaceRecovers) {
  auto e = std::make_unique<TrackEngine>(chicago_receiver());
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  for (std::uint64_t i = 0; i < 20; ++i, t += kSec / 2)
    feed(*e, modes::encode_surface_position(kIcao, 20, 90, modes::cpr_encode(42.947, -87.897, i % 2 == 1, true)), t, sink);
  EXPECT_EQ(e->stats().cpr_reanchored, 1u);
  EXPECT_NEAR(last_lat(sink), 42.947, 1e-3);
}

// A corrupt frame in the first pair of a new track: its fix is unchecked
// (there is nothing to check it against) and lands a whole number of
// longitude zones away. Later good frames of the other format keep pairing
// with the corrupt one and "confirm" the phantom; that confirmation must not
// clear the rejections, or the track stays there.
TEST(TrackEngine, ACorruptFrameInTheFirstPairIsCorrected) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  auto pos = [&](double la, double lo, bool odd) {
    feed(*e, modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(la, lo, odd, false)), t, sink);
    t += kSec / 2;
  };
  pos(40.0, -70.0, false);  // corrupt even creates the track
  pos(40.0, -88.0, true);   // first fix: phantom
  ASSERT_GT(std::abs(last_lon(sink) + 88.0), 10.0);
  for (int i = 0; i < 12; ++i) pos(40.0, -88.0, i % 2 == 0);
  EXPECT_EQ(e->stats().cpr_resets, 1u);
  EXPECT_NEAR(last_lon(sink), -88.0, 1e-3);
  EXPECT_NEAR(last_lat(sink), 40.0, 1e-3);
}

// The same right after a reset (the fix that re-anchors is unchecked, so a
// corrupt frame there gives a phantom too): it is corrected the same way.
TEST(TrackEngine, ACorruptFrameRightAfterAResetIsCorrected) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  auto pos = [&](double la, double lo, bool odd) {
    feed(*e, modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(la, lo, odd, false)), t, sink);
    t += kSec / 2;
  };
  pos(40.0, -88.0, false);
  pos(40.0, -88.0, true);
  pos(40.0, -70.0, false);
  pos(40.0, -70.0, true);
  pos(40.0, -70.0, false);  // three rejections: reset
  ASSERT_EQ(e->stats().cpr_resets, 1u);
  pos(40.0, -88.0, true);
  pos(40.0, -70.0, false);  // corrupt: re-anchors unchecked at a phantom
  ASSERT_EQ(e->stats().cpr_reanchored, 1u);
  ASSERT_GT(std::abs(last_lon(sink) + 88.0), 10.0);
  for (int i = 0; i < 12; ++i) pos(40.0, -88.0, i % 2 == 0);
  EXPECT_EQ(e->stats().cpr_resets, 2u);
  EXPECT_NEAR(last_lon(sink), -88.0, 1e-3);
}

// CPR fields both zero are bogus (readsb's rule): such a frame neither
// creates a position nor pairs. On real traffic one surface frame with CPR
// (0, 0) placed an aircraft parked at Midway 16 NM away for minutes.
TEST(TrackEngine, ZeroCprFieldsAreIgnored) {
  auto e = std::make_unique<TrackEngine>(chicago_receiver());
  CollectSink sink;
  feed(*e, modes::encode_surface_position(kIcao, 5, 90, modes::CprFrame{0, 0, false}), 10 * kSec, sink);
  EXPECT_EQ(e->stats().cpr_rejected, 1u);
  EXPECT_EQ(e->stats().cpr_global + e->stats().cpr_local, 0u);
  feed(*e, modes::encode_surface_position(kIcao, 5, 90, modes::cpr_encode(41.7807, -87.7546, true, true)),
       10 * kSec + kSec / 2, sink);
  EXPECT_EQ(e->stats().cpr_global, 0u) << "no pair with the bogus frame";
  EXPECT_NEAR(last_lat(sink), 41.7807, 1e-3);
}

// Real frames (professor's capture, day 1): ABE2B2 parked at Midway sent a
// surface position with every CPR bit zero first (a placeholder); decoded
// against the receiver it put the aircraft 16 NM north-west, and a first
// version of this change kept it there for 300 s. Ignored now, the real
// frames place it at Midway at once.
TEST(TrackEngine, APlaceholderSurfaceFrameDoesNotPlaceTheAircraft) {
  auto e = std::make_unique<TrackEngine>(chicago_receiver());
  CollectSink sink;
  const std::vector<TimedHex> frames = {{0, "8CABE2B240000000000000011B05"},  // CPR (0, 0)
                                        {440, "8CABE2B24000058EF62548881F2A"},
                                        {984, "8CABE2B24010036A52320EAFB868"},
                                        {1504, "8CABE2B24010058EF425483E76C0"},
                                        {1936, "8CABE2B23810036A52320EE43214"},
                                        {2344, "8CABE2B23810036A52320EE43214"},
                                        {2944, "8CABE2B23810036A52320EE43214"},
                                        {3441, "8CABE2B23810058EF4254875FCBC"},
                                        {3961, "8CABE2B23810036A52320EE43214"},
                                        {4457, "8CABE2B23810058EF4254875FCBC"}};
  for (const auto& f : frames) {
    const auto b = adsb::test::hex(f.hex);
    FrameMeta meta;
    meta.rx_ns = 100 * kSec + f.ms * 1'000'000ULL;
    e->on_frame(b.data(), b.size(), meta, sink);
  }
  bool any = false;
  for (const auto& u : sink.updates) {
    if (!(u.update_mask & protocol::update_mask::kPosition)) continue;
    any = true;
    EXPECT_NEAR(u.lat_e7 / 1e7, 41.7807, 0.002);
    EXPECT_NEAR(u.lon_e7 / 1e7, -87.7546, 0.002);
  }
  EXPECT_TRUE(any);
  EXPECT_EQ(e->stats().cpr_rejected, 1u) << "only the placeholder";
}

// A rejected airborne position does not end "on ground" (readsb drops the
// whole message that gave a bad position).
TEST(TrackEngine, ARejectedAirbornePositionLeavesTheGroundState) {
  auto e = std::make_unique<TrackEngine>(chicago_receiver());
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  // An airborne frame (no partner yet), then the aircraft is seen taxiing.
  feed(*e, modes::encode_airborne_position(kIcao, 1000, modes::cpr_encode(41.7807, -87.7546, false, false)), t, sink);
  t += kSec / 2;
  // (Odd surface frames only, so the airborne even frame keeps its slot.)
  for (std::uint64_t i = 0; i < 4; ++i, t += kSec / 2)
    feed(*e, modes::encode_surface_position(kIcao, 5, 90, modes::cpr_encode(41.7807, -87.7546, true, true)), t, sink);
  ASSERT_TRUE(sink.updates.back().flags & protocol::track_flags::kOnGround);
  // A corrupt odd airborne frame: it pairs with the airborne even one and
  // gives a position 60 km away, which is rejected.
  // CA 7 ("either"): the frame itself says nothing about air/ground.
  modes::Frame f = modes::encode_airborne_position(kIcao, 1000, modes::cpr_encode(41.9, -87.0, true, false));
  f.bytes[0] = static_cast<std::uint8_t>((17u << 3) | 7u);
  modes::write_parity(f.bytes, f.len, 0);
  feed(*e, f, t, sink);
  EXPECT_EQ(e->stats().cpr_rejected, 1u);
  EXPECT_TRUE(sink.updates.back().flags & protocol::track_flags::kOnGround);
}

// A rejected frame is never kept for pairing: after one corrupt frame the
// next frame of the other format still pairs with the last good one.
TEST(TrackEngine, ARejectedFrameIsNotKeptForPairing) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  std::uint64_t t = 10 * kSec;
  auto pos = [&](double la, double lo, bool odd) {
    feed(*e, modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(la, lo, odd, false)), t, sink);
    t += 500'000'000;
  };
  pos(40.0, -88.0, false);
  pos(40.0, -88.0, true);
  ASSERT_EQ(e->stats().cpr_global, 1u);
  pos(10.0, -30.0, false);  // corrupt even: its fix with the stored odd is rejected
  EXPECT_EQ(e->stats().cpr_rejected, 1u);
  pos(40.001, -88.0, true);  // pairs with the good even, not the corrupt one
  EXPECT_EQ(e->stats().cpr_global, 2u);
  EXPECT_EQ(e->stats().cpr_rejected, 1u);
  EXPECT_NEAR(sink.updates.back().lat_e7 / 1e7, 40.001, 1e-3);
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
// Integrity (NIC, ADS-B version), heading and airspeed
// ---------------------------------------------------------------------------

modes::Frame position(double lat, double lon, bool odd, unsigned tc, bool nic_b = false) {
  return modes::encode_airborne_position(kIcao, 35000, modes::cpr_encode(lat, lon, odd, false), tc, 5, nic_b);
}

modes::Frame opstatus(unsigned version, bool nic_a, bool true_north = false, bool surface = false,
                      bool nic_c = false) {
  modes::OperationalStatus os;
  os.version = static_cast<std::uint8_t>(version);
  os.surface = surface;
  os.nic_supplement_a = nic_a;
  os.nic_supplement_c = nic_c;
  os.heading_true_north = true_north;
  return modes::encode_operational_status(kIcao, os);
}

TEST(TrackEngine, NicFollowsTheTypeCodeAndTheSupplementsKnownAtReceipt) {
  namespace um = protocol::update_mask;
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  const double lat = 41.9, lon = -87.9;
  // No TC 31 yet: version 0 rules, adsb_version null. TC 11 = NIC 8.
  feed(*e, position(lat, lon, false, 11, true), 10 * kSec, sink);
  feed(*e, position(lat, lon, true, 11, true), 10 * kSec + kSec / 2, sink);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(sink.updates.back().nic, 8u);
  EXPECT_EQ(sink.updates.back().adsb_version, TrackUpdate::kAdsbVersionNull);
  EXPECT_TRUE(sink.updates.back().update_mask & um::kIntegrity);
  // Version 2 with NIC-A: TC 11 with NIC-B means NIC 9 from now on.
  feed(*e, opstatus(2, true), 11 * kSec, sink);
  EXPECT_EQ(sink.updates.back().adsb_version, 2u);
  EXPECT_EQ(sink.updates.back().update_mask, um::kIntegrity) << "the version changed, nothing else";
  EXPECT_EQ(sink.updates.back().nic, 8u) << "the current position's NIC was fixed at receipt";
  // The next global fix pairs a NIC 9 frame with the NIC 8 one: the weaker counts.
  feed(*e, position(lat + 0.01, lon, false, 11, true), 11 * kSec + kSec / 2, sink);
  EXPECT_EQ(sink.updates.back().nic, 8u);
  feed(*e, position(lat + 0.02, lon, true, 11, true), 12 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 9u);
  EXPECT_TRUE(sink.updates.back().update_mask & um::kIntegrity);
  // Without NIC-B the same type code is NIC 8 in version 2.
  feed(*e, position(lat + 0.03, lon, false, 11, false), 12 * kSec + kSec / 2, sink);
  EXPECT_EQ(sink.updates.back().nic, 8u);
  // Same NIC again: kIntegrity is not set.
  feed(*e, position(lat + 0.04, lon, true, 11, false), 13 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 8u);
  EXPECT_FALSE(sink.updates.back().update_mask & um::kIntegrity);
  // TC 9 is NIC 11 whatever the supplements.
  feed(*e, position(lat + 0.05, lon, false, 9), 13 * kSec + kSec / 2, sink);
  feed(*e, position(lat + 0.06, lon, true, 9), 14 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 11u);
}

TEST(TrackEngine, OperationalStatusFromRebroadcastsOrOutOfOrderIsIgnored) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 10 * kSec, sink);
  // DF18 CF=2 (TIS-B fine format) and CF=6 (ADS-R) are rebroadcasts: their
  // version is the ground station's, not the aircraft's.
  for (const unsigned cf : {2u, 6u}) {
    modes::Frame f = opstatus(2, true);
    f.bytes[0] = static_cast<std::uint8_t>((18u << 3) | cf);
    modes::write_parity(f.bytes, 14, 0);
    feed(*e, f, 11 * kSec, sink);
  }
  ASSERT_FALSE(sink.updates.empty()) << "the DF11 created the track";
  EXPECT_EQ(sink.updates.back().adsb_version, TrackUpdate::kAdsbVersionNull);
  EXPECT_EQ(sink.updates.size(), 1u) << "the rebroadcast statuses changed nothing";
  feed(*e, opstatus(1, false), 12 * kSec, sink);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(sink.updates.back().adsb_version, 1u);
  const std::size_t n = sink.updates.size();
  feed(*e, opstatus(2, true), 11 * kSec + kSec / 2, sink);  // older than the last one
  EXPECT_EQ(sink.updates.size(), n);
  EXPECT_GE(e->stats().out_of_order, 1u);
}

TEST(TrackEngine, HeadingAndAirspeedComeFromVelocitySubtype3) {
  namespace um = protocol::update_mask;
  namespace tf = protocol::track_flags;
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_airspeed_heading(kIcao, 271.4, 452, true, -640), 10 * kSec, sink);
  ASSERT_EQ(sink.updates.size(), 1u);
  TrackUpdate u = sink.updates.back();
  EXPECT_EQ(u.heading_cdeg, 27141u) << "772 x 360/1024 = 271.40625 deg, rounded to 0.01";
  EXPECT_EQ(u.airspeed_kt, 452u);
  EXPECT_TRUE(u.flags & tf::kAirspeedTrue);
  EXPECT_FALSE(u.flags & tf::kHeadingTrue) << "magnetic until a TC 31 says otherwise";
  EXPECT_EQ(u.vrate_fpm, -640);
  EXPECT_EQ(u.gs_dkt, TrackUpdate::kGsDktNull) << "subtype 3 carries no ground speed";
  EXPECT_TRUE(u.update_mask & um::kHeading);
  EXPECT_TRUE(u.update_mask & um::kAirspeed);
  // Indicated airspeed, same value: only the flag changes, under kAirspeed.
  feed(*e, modes::encode_airspeed_heading(kIcao, 271.4, 452, false, -640), 11 * kSec, sink);
  u = sink.updates.back();
  EXPECT_FALSE(u.flags & tf::kAirspeedTrue);
  EXPECT_EQ(u.update_mask, um::kAirspeed);
  // HRD = true north (version 2): the reference changes under kHeading.
  feed(*e, opstatus(2, false, /*true_north=*/true), 12 * kSec, sink);
  u = sink.updates.back();
  EXPECT_TRUE(u.flags & tf::kHeadingTrue);
  EXPECT_EQ(u.update_mask, um::kHeading | um::kIntegrity);
  // A version 0 status carries no HRD: the reference stays.
  feed(*e, opstatus(0, false), 13 * kSec, sink);
  EXPECT_TRUE(sink.updates.back().flags & tf::kHeadingTrue);
  EXPECT_EQ(sink.updates.back().adsb_version, 0u);
  // No heading in the message: the last one stays; airspeed updates.
  feed(*e, modes::encode_airspeed_heading(kIcao, -1, 460, false, -640), 14 * kSec, sink);
  u = sink.updates.back();
  EXPECT_NEAR(u.heading_cdeg / 100.0, 271.4, 360.0 / 1024);
  EXPECT_EQ(u.airspeed_kt, 460u);
  EXPECT_EQ(u.update_mask, um::kAirspeed);
}

TEST(TrackEngine, AirspeedAndHeadingWithoutVerticalRateAreApplied) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  modes::Frame f = modes::encode_airspeed_heading(kIcao, 90, 300, true, 0);
  // Clear the vertical rate (ME bits 38-46 = frame bits 70-78: "no information").
  for (unsigned bit = 69; bit < 78; ++bit) f.bytes[bit / 8] = static_cast<std::uint8_t>(f.bytes[bit / 8] & ~(0x80u >> (bit % 8)));
  modes::write_parity(f.bytes, 14, 0);
  modes::DecodedMsg m;
  ASSERT_EQ(modes::decode(f.bytes, 14, m), modes::DecodeStatus::kOk);
  ASSERT_FALSE(m.has(modes::kFieldVerticalRate));
  feed(*e, f, 10 * kSec, sink);
  ASSERT_EQ(sink.updates.size(), 1u);
  EXPECT_EQ(sink.updates.back().heading_cdeg, 9000u);
  EXPECT_EQ(sink.updates.back().airspeed_kt, 300u);
}

TEST(TrackEngine, HeadingReferenceAloneIsNotAnUpdate) {
  // HRD before any heading: the flag is kept for later, but nothing changed
  // that a subscriber can see except the version.
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 10 * kSec, sink);
  feed(*e, opstatus(2, false, /*true_north=*/true), 11 * kSec, sink);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(sink.updates.back().update_mask, protocol::update_mask::kIntegrity);
  EXPECT_EQ(sink.updates.back().heading_cdeg, TrackUpdate::kHeadingCdegNull);
  EXPECT_FALSE(sink.updates.back().flags & protocol::track_flags::kHeadingTrue) << "no heading, no reference";
  // A status that only flips HRD while there is no heading changes nothing visible.
  const std::size_t n = sink.updates.size();
  feed(*e, opstatus(2, false, /*true_north=*/false), 11 * kSec + kSec / 2, sink);
  feed(*e, opstatus(2, false, /*true_north=*/true), 11 * kSec + 3 * kSec / 4, sink);
  EXPECT_EQ(sink.updates.size(), n);
  feed(*e, modes::encode_airspeed_heading(kIcao, 90, 300, true, 0), 12 * kSec, sink);
  EXPECT_TRUE(sink.updates.back().flags & protocol::track_flags::kHeadingTrue);
}

TEST(TrackEngine, SurfaceNicUsesNicC) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  const double lat = 40.04, lon = -88.28;
  auto surf = [&](bool odd, double dlat) {
    return modes::encode_surface_position(kIcao, 10, 90, modes::cpr_encode(lat + dlat, lon, odd, true), 8, 4);
  };
  EngineConfig cfg;
  cfg.has_receiver = true;
  cfg.receiver_lat = lat;
  cfg.receiver_lon = lon;
  e = std::make_unique<TrackEngine>(cfg);
  feed(*e, opstatus(2, true, false, /*surface=*/true, /*nic_c=*/true), 10 * kSec, sink);
  feed(*e, surf(false, 0), 10 * kSec + kSec / 2, sink);
  feed(*e, surf(true, 0.00001), 11 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 7u) << "TC 8 with NIC-A and NIC-C";
  feed(*e, opstatus(2, true, false, true, false), 12 * kSec, sink);
  feed(*e, surf(false, 0.00002), 12 * kSec + kSec / 2, sink);
  feed(*e, surf(true, 0.00003), 13 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 6u) << "TC 8 with NIC-A only";
  // A status with capability class code != 0 does not carry NIC-C: the last one stays.
  feed(*e, opstatus(2, true, false, true, true), 14 * kSec, sink);
  modes::OperationalStatus os;
  os.surface = true;
  os.version = 2;
  os.nic_supplement_a = true;
  os.capability = 0x400;  // class code 1, bit 20 clear
  feed(*e, modes::encode_operational_status(kIcao, os), 14 * kSec + kSec / 4, sink);
  feed(*e, surf(false, 0.00004), 14 * kSec + kSec / 2, sink);
  feed(*e, surf(true, 0.00005), 15 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 7u);
  // NIC-C expires a minute after the status that set it (NIC-A was refreshed since).
  feed(*e, opstatus(2, true, false, true, false), 15 * kSec + kSec / 4, sink);  // refreshes A, sets C false
  feed(*e, opstatus(2, true, false, true, true), 16 * kSec, sink);              // C true at 16 s
  os.capability = 0x400;
  for (std::uint64_t t = 20; t <= 80; t += 20) {  // A refreshed, C not (class code 1)
    feed(*e, modes::encode_operational_status(kIcao, os), t * kSec, sink);
  }
  feed(*e, surf(false, 0.00006), 76 * kSec + kSec / 2, sink);
  feed(*e, surf(true, 0.00007), 77 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 6u) << "61 s after NIC-C was last set";
}

TEST(TrackEngine, ALocalFixAgainstTheTrackKeepsTheWeakerNic) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  const double lat = 41.9, lon = -87.9;
  feed(*e, position(lat, lon, false, 11, true), 10 * kSec, sink);
  feed(*e, position(lat, lon, true, 11, true), 10 * kSec + kSec / 2, sink);
  ASSERT_EQ(sink.updates.back().nic, 8u);
  feed(*e, opstatus(2, true), 11 * kSec, sink);
  // Odd frames lost for longer than the pair window: the next even frame
  // (NIC 9 on its own) is decoded locally against the track, as in readsb.
  feed(*e, position(lat + 0.05, lon, false, 11, true), 22 * kSec, sink);
  EXPECT_EQ(e->stats().cpr_local, 1u);
  EXPECT_EQ(sink.updates.back().nic, 8u) << "min(this frame, the track's position)";
  // A global fix from two NIC 9 frames raises it.
  feed(*e, position(lat + 0.06, lon, true, 11, true), 22 * kSec + kSec / 2, sink);
  feed(*e, position(lat + 0.07, lon, false, 11, true), 23 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nic, 9u);
}

TEST(TrackEngine, NicSupplementsExpireAMinuteAfterTheLastStatus) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  const double lat = 41.9, lon = -87.9;
  feed(*e, opstatus(2, true), 10 * kSec, sink);
  double d = 0;
  std::uint64_t t = 10 * kSec;
  bool odd = false;
  auto step = [&](std::uint64_t to) {
    for (; t < to; t += kSec / 2) {
      feed(*e, position(lat + d, lon, odd, 11, true), t, sink);
      odd = !odd;
      d += 0.001;
    }
  };
  step(69 * kSec);
  EXPECT_EQ(sink.updates.back().nic, 9u) << "59 s after the status";
  step(72 * kSec);
  EXPECT_EQ(sink.updates.back().nic, 8u) << "NIC-A no longer counts";
  EXPECT_EQ(sink.updates.back().adsb_version, 2u) << "the version does not expire (readsb keeps it too)";
  feed(*e, opstatus(2, true), t, sink);
  step(t + 2 * kSec);
  EXPECT_EQ(sink.updates.back().nic, 9u);
}

TEST(TrackEngine, NucpForVersion0AircraftFromTheTypeCode) {
  namespace um = protocol::update_mask;
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  const double lat = 41.9, lon = -87.9;
  // No TC 31: version 0. TC 11 -> NUCp 7; a pair with TC 13 (NUCp 5): the weaker.
  feed(*e, position(lat, lon, false, 11), 10 * kSec, sink);
  feed(*e, position(lat, lon, true, 11), 10 * kSec + kSec / 2, sink);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(sink.updates.back().nuc_p, 7u);
  feed(*e, position(lat + 0.01, lon, false, 13), 11 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nuc_p, 5u);
  EXPECT_TRUE(sink.updates.back().update_mask & um::kIntegrity);
  // A version 2 status: later positions carry a NIC, not a NUCp.
  feed(*e, opstatus(2, false), 11 * kSec + kSec / 2, sink);
  EXPECT_EQ(sink.updates.back().nuc_p, 5u) << "the current position's NUCp stands until the next fix";
  feed(*e, position(lat + 0.02, lon, true, 13), 12 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nuc_p, 0u);
  EXPECT_GT(sink.updates.back().nic, 0u);
}

TEST(TrackEngine, NacpAndSilFromOperationalStatusAndTargetState) {
  namespace um = protocol::update_mask;
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 10 * kSec, sink);
  modes::OperationalStatus os;
  os.version = 2;
  os.nac_p = 10;
  os.sil = 3;
  os.sil_per_sample = true;
  feed(*e, modes::encode_operational_status(kIcao, os), 11 * kSec, sink);
  ASSERT_FALSE(sink.updates.empty());
  EXPECT_EQ(sink.updates.back().nac_p, 10u);
  EXPECT_EQ(sink.updates.back().sil, 3u | 4u);
  EXPECT_TRUE(sink.updates.back().update_mask & um::kIntegrity);
  // Version 1 has no SIL supplement: per hour, whatever bit 55 says.
  os.version = 1;
  os.nac_p = 9;
  os.sil = 2;
  feed(*e, modes::encode_operational_status(kIcao, os), 12 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nac_p, 9u);
  EXPECT_EQ(sink.updates.back().sil, 2u);
  // An older status arriving late does not go back.
  os.nac_p = 4;
  feed(*e, modes::encode_operational_status(kIcao, os), 11 * kSec + kSec / 2, sink);
  EXPECT_EQ(sink.updates.back().nac_p, 9u);
  // From a TIS-B rebroadcast (DF18 CF 2): ignored.
  os.nac_p = 3;
  const std::size_t before = sink.updates.size();
  modes::Frame tisb = modes::encode_operational_status(kIcao, os);
  tisb.bytes[0] = static_cast<std::uint8_t>((18u << 3) | 2u);
  modes::write_parity(tisb.bytes, 14, 0);
  feed(*e, tisb, 13 * kSec, sink);
  EXPECT_EQ(sink.updates.size(), before) << "the rebroadcast changed nothing";
  // Target state (TC 29, published frame, NACp 9 SIL 3 per hour) of another aircraft.
  auto e2 = std::make_unique<TrackEngine>();
  CollectSink s2;
  const auto ts = adsb::test::hex("8DA05629EA21485CBF3F8CADAEEB");
  FrameMeta meta;
  meta.rx_ns = 20 * kSec;
  e2->on_frame(ts.data(), ts.size(), meta, s2);
  ASSERT_FALSE(s2.updates.empty());
  EXPECT_EQ(s2.updates.back().icao, 0xA05629u);
  EXPECT_EQ(s2.updates.back().nac_p, 9u);
  EXPECT_EQ(s2.updates.back().sil, 3u);
  // The same target state rebroadcast as TIS-B (DF18 CF 2): ignored.
  auto e3 = std::make_unique<TrackEngine>();
  CollectSink s3;
  feed(*e3, modes::encode_df11(0xA05629), 19 * kSec, s3);
  auto tisb_ts = ts;
  tisb_ts[0] = static_cast<std::uint8_t>((18u << 3) | 2u);
  modes::write_parity(tisb_ts.data(), 14, 0);
  meta.rx_ns = 20 * kSec;
  e3->on_frame(tisb_ts.data(), tisb_ts.size(), meta, s3);
  EXPECT_EQ(s3.updates.back().nac_p, 0u);
  // Only NACp changes (same version): kIntegrity, and nothing else.
  modes::OperationalStatus v2;
  v2.version = 2;
  v2.nac_p = 9;
  v2.sil = 3;
  feed(*e2, modes::encode_operational_status(0xA05629, v2), 21 * kSec, s2);
  v2.nac_p = 11;
  feed(*e2, modes::encode_operational_status(0xA05629, v2), 22 * kSec, s2);
  EXPECT_EQ(s2.updates.back().nac_p, 11u);
  EXPECT_EQ(s2.updates.back().update_mask, protocol::update_mask::kIntegrity);
  // A status older than the newest target state (it passes the status
  // freshness check: the last status is older still) does not go back.
  meta.rx_ns = 30 * kSec;
  e2->on_frame(ts.data(), ts.size(), meta, s2);  // TC 29 again: NACp 9
  ASSERT_EQ(s2.updates.back().nac_p, 9u);
  v2.nac_p = 4;
  feed(*e2, modes::encode_operational_status(0xA05629, v2), 25 * kSec, s2);
  EXPECT_EQ(s2.updates.back().nac_p, 9u);
}

modes::Frame bds60(double hdg, double ias) {
  return modes::encode_df20(kIcao, 31000, modes::comm_b_bds60(hdg, ias, 0.7, 0, 0));
}

TEST(TrackEngine, CommBAirDataNeedsTwoAgreeingRepliesAndNoAdsbAirData) {
  namespace tf = protocol::track_flags;
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 10 * kSec, sink);
  ASSERT_EQ(e->track_count(), 1u);
  // One reply: not yet.
  feed(*e, bds60(120.0, 250), 11 * kSec, sink);
  EXPECT_EQ(sink.updates.back().heading_cdeg, TrackUpdate::kHeadingCdegNull);
  // A disagreeing one: still not (and it becomes the one to confirm).
  feed(*e, bds60(200.0, 250), 12 * kSec, sink);
  EXPECT_EQ(sink.updates.back().heading_cdeg, TrackUpdate::kHeadingCdegNull);
  // Agreeing with the previous one (within 10 degrees, 20 kt): applied.
  feed(*e, bds60(203.0, 262), 13 * kSec, sink);
  const TrackUpdate& u = sink.updates.back();
  EXPECT_NEAR(u.heading_cdeg, 20300, 20);
  EXPECT_EQ(u.airspeed_kt, 262u);
  EXPECT_TRUE(u.flags & tf::kAirDataCommB);
  EXPECT_FALSE(u.flags & tf::kHeadingTrue) << "BDS 6,0 heading is magnetic";
  EXPECT_FALSE(u.flags & tf::kAirspeedTrue) << "indicated";
  // Across north: 358 and 3 degrees agree.
  feed(*e, bds60(358.0, 262), 14 * kSec, sink);
  feed(*e, bds60(3.0, 262), 15 * kSec, sink);
  EXPECT_NEAR(sink.updates.back().heading_cdeg, 300, 20);
  // Too far apart in time (> 30 s): the pair does not confirm, even agreeing.
  feed(*e, bds60(5.0, 263), 50 * kSec, sink);
  EXPECT_NEAR(sink.updates.back().heading_cdeg, 300, 20);
  // ADS-B air data takes over and clears the Comm-B flag ...
  feed(*e, modes::encode_airspeed_heading(kIcao, 45.0, 410, true, 0), 51 * kSec, sink);
  EXPECT_NEAR(sink.updates.back().heading_cdeg, 4500, 20);
  EXPECT_FALSE(sink.updates.back().flags & tf::kAirDataCommB);
  // ... and Comm-B is ignored while it is fresh (30 s) ...
  feed(*e, bds60(91.0, 300), 52 * kSec, sink);
  feed(*e, bds60(92.0, 300), 53 * kSec, sink);
  EXPECT_NEAR(sink.updates.back().heading_cdeg, 4500, 20);
  // ... and used again once it is not.
  feed(*e, bds60(93.0, 300), 82 * kSec, sink);
  feed(*e, bds60(94.0, 301), 83 * kSec, sink);
  EXPECT_NEAR(sink.updates.back().heading_cdeg, 9400, 20);
  EXPECT_TRUE(sink.updates.back().flags & tf::kAirDataCommB);
}

// Found by review: ADS-B air data with only one of the two fields used to
// relabel the other (still Comm-B's magnetic heading) as ADS-B, and a TC 31
// then marked it true north.
TEST(TrackEngine, PartialAdsbAirDataDropsTheCommBFieldItDoesNotReplace) {
  namespace tf = protocol::track_flags;
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, modes::encode_df11(kIcao), 10 * kSec, sink);
  feed(*e, bds60(120.0, 250), 11 * kSec, sink);
  feed(*e, bds60(121.0, 251), 12 * kSec, sink);
  ASSERT_TRUE(sink.updates.back().flags & tf::kAirDataCommB);
  ASSERT_NE(sink.updates.back().heading_cdeg, TrackUpdate::kHeadingCdegNull);
  // Subtype 3 with IAS and no heading.
  feed(*e, modes::encode_airspeed_heading(kIcao, -1, 300, false, 0), 13 * kSec, sink);
  {
    const TrackUpdate& u = sink.updates.back();
    EXPECT_EQ(u.airspeed_kt, 300u);
    EXPECT_EQ(u.heading_cdeg, TrackUpdate::kHeadingCdegNull) << "Comm-B's heading must not pass as ADS-B";
    EXPECT_FALSE(u.flags & tf::kAirDataCommB);
    EXPECT_TRUE(u.update_mask & protocol::update_mask::kHeading);
  }
  // A true-north TC 31 afterwards: no heading, so no reference shown.
  feed(*e, opstatus(2, false, true), 14 * kSec, sink);
  EXPECT_FALSE(sink.updates.back().flags & tf::kHeadingTrue);
  EXPECT_EQ(sink.updates.back().heading_cdeg, TrackUpdate::kHeadingCdegNull);
  // Both fields from ADS-B: nothing dropped.
  feed(*e, bds60(122.0, 252), 50 * kSec, sink);
  feed(*e, bds60(123.0, 253), 51 * kSec, sink);
  ASSERT_TRUE(sink.updates.back().flags & tf::kAirDataCommB);
  feed(*e, modes::encode_airspeed_heading(kIcao, 200.0, 310, false, 0), 52 * kSec, sink);
  EXPECT_NEAR(sink.updates.back().heading_cdeg, 20000, 20);
  EXPECT_EQ(sink.updates.back().airspeed_kt, 310u);
  EXPECT_TRUE(sink.updates.back().flags & tf::kHeadingTrue) << "ADS-B heading, true north per the TC 31";
}

TEST(TrackEngine, AVersion0OrReservedStatusClearsNacpAndSilAndGivesNucp) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  modes::OperationalStatus os;
  os.version = 2;
  os.nac_p = 10;
  os.sil = 3;
  feed(*e, modes::encode_operational_status(kIcao, os), 10 * kSec, sink);
  ASSERT_EQ(sink.updates.back().nac_p, 10u);
  os.version = 3;  // reserved
  feed(*e, modes::encode_operational_status(kIcao, os), 11 * kSec, sink);
  EXPECT_EQ(sink.updates.back().nac_p, 0u);
  EXPECT_EQ(sink.updates.back().sil, 0u);
  EXPECT_TRUE(sink.updates.back().update_mask & protocol::update_mask::kIntegrity);
  // Read as version 0: positions get a NUCp (TC 12 -> 6), as they get a version 0 NIC.
  feed(*e, position(41.9, -87.9, false, 12), 12 * kSec, sink);
  feed(*e, position(41.9, -87.9, true, 12), 12 * kSec + kSec / 2, sink);
  EXPECT_EQ(sink.updates.back().nuc_p, 6u);
}

// A Comm-B heading is magnetic: it never carries the true-north flag, not
// from an earlier ADS-B heading nor from a later TC 31.
TEST(TrackEngine, CommBHeadingIsNeverMarkedTrueNorth) {
  namespace tf = protocol::track_flags;
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, opstatus(2, false, true), 10 * kSec, sink);  // HRD true north
  feed(*e, modes::encode_airspeed_heading(kIcao, 90.0, 300, false, 0), 11 * kSec, sink);
  ASSERT_TRUE(sink.updates.back().flags & tf::kHeadingTrue);
  // ADS-B air data goes stale; Comm-B takes over.
  feed(*e, bds60(95.0, 300), 50 * kSec, sink);
  feed(*e, bds60(96.0, 301), 51 * kSec, sink);
  ASSERT_TRUE(sink.updates.back().flags & tf::kAirDataCommB);
  EXPECT_FALSE(sink.updates.back().flags & tf::kHeadingTrue);
  feed(*e, opstatus(2, false, true), 52 * kSec, sink);
  EXPECT_FALSE(sink.updates.back().flags & tf::kHeadingTrue) << "a TC 31 does not make it true north";
  EXPECT_TRUE(sink.updates.back().flags & tf::kAirDataCommB);
}

TEST(TrackEngine, ReservedVersionsAreReportedButReadAsVersion0) {
  auto e = std::make_unique<TrackEngine>();
  CollectSink sink;
  feed(*e, opstatus(3, true, /*true_north=*/true), 10 * kSec, sink);
  feed(*e, modes::encode_airspeed_heading(kIcao, 90, 300, true, 0), 10 * kSec + kSec / 4, sink);
  feed(*e, position(41.9, -87.9, false, 16, true), 10 * kSec + kSec / 2, sink);
  feed(*e, position(41.9, -87.9, true, 16, true), 11 * kSec, sink);
  const TrackUpdate& u = sink.updates.back();
  EXPECT_EQ(u.adsb_version, 3u);
  EXPECT_EQ(u.nic, 2u) << "NIC-A of a reserved version is not read";
  EXPECT_FALSE(u.flags & protocol::track_flags::kHeadingTrue);
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

TEST_F(EndToEnd, NicHeadingAndAirspeedMatchTheSimulatedAvionics) {
  sim::SynthConfig cfg = config();
  cfg.avionics = true;
  cfg.aircraft = 300;
  cfg.ground_fraction = 0.2;
  cfg.airspeed_fraction = 0.4;
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
    m.line = r.line;
    e->on_frame(r.payload, r.len, m, sink);
  }
  std::map<std::uint32_t, const sim::AircraftInfo*> info;
  for (const auto& a : s.aircraft) info[a.icao] = &a;
  std::map<std::uint32_t, std::vector<const sim::TruthPoint*>> truth;
  for (const auto& t : s.truth) truth[t.icao].push_back(&t);
  std::map<std::uint32_t, TrackUpdate> last;
  for (const auto& u : sink.updates) last[u.icao] = u;

  // Every combination in the generator's table shows up.
  std::set<std::uint8_t> nics, versions;
  int nic_checked = 0, hdg_checked = 0;
  for (const auto& [icao, u] : last) {
    const auto* a = info.at(icao);
    if (u.lat_e7 == TrackUpdate::kLatE7Null) continue;
    // Version 0 has no TC 31; versions 1/2 are known after 90 s.
    if (a->adsb_version == 0) {
      EXPECT_EQ(u.adsb_version, TrackUpdate::kAdsbVersionNull) << std::hex << icao;
    } else {
      ASSERT_EQ(u.adsb_version, a->adsb_version) << std::hex << icao;
      // The reference is shown with a heading only.
      const bool shown = a->airspeed_velocity && a->heading_true_north;
      EXPECT_EQ((u.flags & protocol::track_flags::kHeadingTrue) != 0, shown) << std::hex << icao;
    }
    ASSERT_EQ(u.nic, a->nic) << std::hex << icao << " tc " << int(a->position_tc) << " v" << int(a->adsb_version);
    // Accuracy: NACp and SIL from the generator's TC 31 (versions 1/2),
    // NUCp from the type code (version 0).
    if (a->adsb_version == 0) {
      EXPECT_EQ(u.nac_p, 0u) << std::hex << icao;
      EXPECT_EQ(u.nuc_p, modes::position_nuc(a->position_tc)) << std::hex << icao;
    } else {
      EXPECT_EQ(u.nac_p, a->nic >= 8 ? 9u : 7u) << std::hex << icao;
      EXPECT_EQ(u.sil, 3u) << std::hex << icao;
      EXPECT_EQ(u.nuc_p, 0u) << std::hex << icao;
    }
    nics.insert(u.nic);
    versions.insert(a->adsb_version);
    ++nic_checked;
  }
  EXPECT_GT(nic_checked, 200);
  EXPECT_EQ(versions.size(), 3u);
  EXPECT_GE(nics.size(), 10u) << "NIC 0-11 nearly all present";

  // Heading and airspeed against the truth at the nearest position (velocity
  // frames are up to ~0.6 s away: turns <= 3 deg/s, plus 0.35 deg steps).
  for (const auto& u : sink.updates) {
    if (!(u.update_mask & protocol::update_mask::kHeading) || u.heading_cdeg == TrackUpdate::kHeadingCdegNull) continue;
    const auto* a = info.at(u.icao);
    ASSERT_TRUE(a->airspeed_velocity) << std::hex << u.icao;
    const auto& pts = truth[u.icao];
    const auto it = std::upper_bound(pts.begin(), pts.end(), u.event_ts_ns,
                                     [](std::uint64_t t, const sim::TruthPoint* p) { return t < p->tx_ns; });
    if (it == pts.begin()) continue;
    const sim::TruthPoint& tp = **(it - 1);
    ASSERT_NEAR(std::remainder(u.heading_cdeg / 100.0 - tp.heading_deg, 360.0), 0.0, 2.5) << std::hex << u.icao;
    ASSERT_NEAR(u.airspeed_kt, tp.airspeed_kt, 1.5) << std::hex << u.icao;
    ASSERT_EQ((u.flags & protocol::track_flags::kAirspeedTrue) != 0, a->airspeed_true);
    ++hdg_checked;
  }
  EXPECT_GT(hdg_checked, 1000);
  for (const auto& [icao, u] : last) {
    if (!info.at(icao)->airspeed_velocity) {
      EXPECT_EQ(u.heading_cdeg, TrackUpdate::kHeadingCdegNull);
      EXPECT_EQ(u.airspeed_kt, TrackUpdate::kAirspeedKtNull);
    }
  }
  EXPECT_EQ(e->stats().cpr_rejected, 0u);
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
