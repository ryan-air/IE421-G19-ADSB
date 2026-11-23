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
