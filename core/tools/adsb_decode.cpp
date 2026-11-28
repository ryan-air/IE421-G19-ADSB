// adsb-decode: print what the decoder makes of every frame of a recording,
// one JSON object per line, for comparison with other decoders
// (bench/crosscheck/ compares it with pyModeS).
//
//   adsb-decode IN.adsbrec [-o OUT.jsonl] [--fix] [--limit N]
//
// Each line is one record, in recording order:
//   {"i":0,"t":12.345678901,"hex":"8D...","st":"ok","df":17,"icao":"A1B2C3",
//    "tc":11, ...decoded fields..., "pos":[lat,lon,"global"|"local"]}
// Fields appear only when the decoder filled them (DecodedMsg::fields), with
// the decoder's own units (ft, kt, deg, ft/min). "t" is rx_ns in seconds.
// "nuc" is modes::position_nuc() of a position message's type code.
//
// Positions need state: every frame is also given to the track engine
// (default configuration, the recording's receiver position), and "pos" is
// the position the engine accepted from this frame (a global even/odd fix or
// a local one), if any. The other fields are the stateless decoder's alone:
// no Address/Parity check against known aircraft, no altitude plausibility.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "adsb/common/build_config.hpp"
#include "adsb/modes/decoder.hpp"
#include "adsb/record/recording.hpp"
#include "adsb/state/track_engine.hpp"

namespace {

[[noreturn]] void usage(int code) {
  std::fprintf(code ? stderr : stdout,
               "usage: adsb-decode IN.adsbrec [-o OUT.jsonl] [--fix] [--limit N]\n"
               "  -o, --out FILE   output (default stdout)\n"
               "  --fix            repair single-bit errors in DF17/18 (decoder and engine)\n"
               "  --limit N        stop after N records\n");
  std::exit(code);
}

struct NullSink {
  void on_update(const adsb::protocol::TrackUpdate&) noexcept {}
  void on_delete(const adsb::protocol::TrackDelete&) noexcept {}
};

// Callsign characters are A-Z, 0-9, space and '#'-style placeholders; anything
// that would need escaping in JSON is printed as '?'.
void put_callsign(std::FILE* out, const char* key, const char (&cs)[8]) {
  char s[9];
  for (int k = 0; k < 8; ++k) {
    const char c = cs[k];
    s[k] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? c : '?';
  }
  int n = 8;
  while (n > 0 && s[n - 1] == ' ') --n;
  s[n] = '\0';
  std::fprintf(out, ",\"%s\":\"%s\"", key, s);
}

void put_target(std::FILE* out, const adsb::modes::TargetState& t) {
  using T = adsb::modes::TargetState;
  std::fprintf(out, ",\"tgt\":{\"tcas\":%d,\"nacp\":%u,\"nicbaro\":%d,\"sil\":%u", t.tcas_operational ? 1 : 0,
               static_cast<unsigned>(t.nac_p), t.nic_baro ? 1 : 0, static_cast<unsigned>(t.sil));
  if (t.valid & T::kAltitude)
    std::fprintf(out, ",\"alt\":%" PRId32 ",\"fms\":%d", t.selected_altitude_ft, t.altitude_from_fms ? 1 : 0);
  if (t.valid & T::kBaro) std::fprintf(out, ",\"baro\":%.1f", static_cast<double>(t.baro_setting_hpa));
  if (t.valid & T::kHeading) std::fprintf(out, ",\"hdg\":%.4f", static_cast<double>(t.selected_heading_deg));
  if (t.valid & T::kModes)
    std::fprintf(out, ",\"ap\":%d,\"vnav\":%d,\"hold\":%d,\"app\":%d,\"lnav\":%d", t.autopilot ? 1 : 0,
                 t.vnav ? 1 : 0, t.altitude_hold ? 1 : 0, t.approach ? 1 : 0, t.lnav ? 1 : 0);
  std::fputc('}', out);
}

void put_op_status(std::FILE* out, const adsb::modes::OperationalStatus& o) {
  std::fprintf(out, ",\"op\":{\"surface\":%d,\"version\":%u", o.surface ? 1 : 0, static_cast<unsigned>(o.version));
  if (o.version == 1 || o.version == 2) {
    std::fprintf(out, ",\"nica\":%d,\"nacp\":%u,\"sil\":%u", o.nic_supplement_a ? 1 : 0, static_cast<unsigned>(o.nac_p),
                 static_cast<unsigned>(o.sil));
    if (!o.surface) std::fprintf(out, ",\"nicbaro\":%d", o.nic_baro ? 1 : 0);
    if (o.version == 2 && !o.surface) std::fprintf(out, ",\"gva\":%u", static_cast<unsigned>(o.gva));
    if (o.nic_supplement_c_valid) std::fprintf(out, ",\"nicc\":%d", o.nic_supplement_c ? 1 : 0);
    std::fprintf(out, ",\"hrd_true\":%d", o.heading_true_north ? 1 : 0);
  }
  std::fputc('}', out);
}

void put_comm_b(std::FILE* out, const adsb::modes::CommB& b) {
  using B = adsb::modes::CommB;
  std::fprintf(out, ",\"commb\":{\"cand\":%u,\"bds\":\"%x,%x\"", static_cast<unsigned>(b.candidates),
               static_cast<unsigned>(b.bds >> 4), static_cast<unsigned>(b.bds & 0xF));
  if (b.valid & B::kCallsign) put_callsign(out, "cs", b.callsign);
  if (b.valid & B::kMcpAltitude) std::fprintf(out, ",\"mcp\":%" PRId32, b.mcp_altitude_ft);
  if (b.valid & B::kFmsAltitude) std::fprintf(out, ",\"fms\":%" PRId32, b.fms_altitude_ft);
  if (b.valid & B::kBaroSetting) std::fprintf(out, ",\"baro\":%.1f", static_cast<double>(b.baro_setting_hpa));
  if (b.valid & B::kRoll) std::fprintf(out, ",\"roll\":%.4f", static_cast<double>(b.roll_deg));
  if (b.valid & B::kTrueTrack) std::fprintf(out, ",\"trk\":%.4f", static_cast<double>(b.true_track_deg));
  if (b.valid & B::kGroundSpeed) std::fprintf(out, ",\"gs\":%.2f", static_cast<double>(b.ground_speed_kt));
  if (b.valid & B::kTrackRate) std::fprintf(out, ",\"rtrk\":%.4f", static_cast<double>(b.track_rate_deg_s));
  if (b.valid & B::kTrueAirspeed) std::fprintf(out, ",\"tas\":%.2f", static_cast<double>(b.true_airspeed_kt));
  if (b.valid & B::kMagneticHeading) std::fprintf(out, ",\"hdg\":%.4f", static_cast<double>(b.magnetic_heading_deg));
  if (b.valid & B::kIndicatedAirspeed) std::fprintf(out, ",\"ias\":%.2f", static_cast<double>(b.indicated_airspeed_kt));
  if (b.valid & B::kMach) std::fprintf(out, ",\"mach\":%.4f", static_cast<double>(b.mach));
  if (b.valid & B::kBaroVerticalRate) std::fprintf(out, ",\"vrb\":%" PRId32, b.baro_vertical_rate_fpm);
  if (b.valid & B::kInertialVerticalRate) std::fprintf(out, ",\"vri\":%" PRId32, b.inertial_vertical_rate_fpm);
  std::fputc('}', out);
}

}  // namespace

int main(int argc, char** argv) {
  std::string in, out_path;
  bool fix = false;
  std::uint64_t limit = UINT64_MAX;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-h" || a == "--help") usage(0);
    if ((a == "-o" || a == "--out") && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--fix") {
      fix = true;
    } else if (a == "--limit" && i + 1 < argc) {
      char* end = nullptr;
      limit = std::strtoull(argv[++i], &end, 10);
      if (*end != '\0') usage(1);
    } else if (!a.empty() && a[0] != '-' && in.empty()) {
      in = a;
    } else {
      usage(1);
    }
  }
  if (in.empty()) usage(1);

  adsb::record::RecordingReader rd;
  std::string err;
  if (!rd.open(in, &err)) {
    std::fprintf(stderr, "adsb-decode: %s\n", err.c_str());
    return 1;
  }
  std::FILE* out = stdout;
  if (!out_path.empty()) {
    out = std::fopen(out_path.c_str(), "w");
    if (!out) {
      std::fprintf(stderr, "adsb-decode: cannot write %s\n", out_path.c_str());
      return 1;
    }
  }
  static char buf[1 << 20];
  std::setvbuf(out, buf, _IOFBF, sizeof buf);

  adsb::state::EngineConfig ecfg;
  ecfg.fix_single_bit = fix;
  if (rd.info().has_receiver_position) {
    ecfg.has_receiver = true;
    ecfg.receiver_lat = rd.info().receiver_lat;
    ecfg.receiver_lon = rd.info().receiver_lon;
  }
  auto engine = std::make_unique<adsb::DefaultTrackEngine>(ecfg);
  NullSink sink;
  const adsb::modes::DecodeOptions opts{fix};

  adsb::record::Record r;
  constexpr std::uint64_t kNsPerS = 1'000'000'000;
  std::uint64_t i = 0;
  while (i < limit && rd.next(r)) {
    std::fprintf(out, "{\"i\":%" PRIu64 ",\"t\":%" PRIu64 ".%09" PRIu64 ",\"hex\":\"", i, r.rx_ns / kNsPerS,
                 r.rx_ns % kNsPerS);
    for (unsigned k = 0; k < r.len; ++k) std::fprintf(out, "%02X", r.payload[k]);
    std::fputc('"', out);
    ++i;
    if (r.len == 2) {
      std::fputs(",\"st\":\"mode_ac\"}\n", out);
      continue;
    }
    adsb::modes::DecodedMsg m;
    const adsb::modes::DecodeStatus st = adsb::modes::decode(r.payload, r.len, m, opts);
    std::fprintf(out, ",\"st\":\"%s\",\"df\":%u", adsb::modes::to_string(st), static_cast<unsigned>(m.df));
    if (st == adsb::modes::DecodeStatus::kOk || st == adsb::modes::DecodeStatus::kAddressParity) {
      std::fprintf(out, ",\"icao\":\"%06" PRIX32 "\",\"ca\":%u", m.icao, static_cast<unsigned>(m.ca));
      if (m.corrected_bit >= 0) std::fprintf(out, ",\"fixed_bit\":%d", static_cast<int>(m.corrected_bit));
      if (m.df == 17 || m.df == 18)
        std::fprintf(out, ",\"tc\":%u,\"sub\":%u", static_cast<unsigned>(m.tc), static_cast<unsigned>(m.subtype));
      if (m.has(adsb::modes::kFieldAltitude))
        std::fprintf(out, ",\"alt\":%" PRId32 ",\"gnss\":%d", m.altitude_ft, m.altitude_gnss ? 1 : 0);
      if (m.has(adsb::modes::kFieldCpr))
        std::fprintf(out, ",\"cpr\":[%" PRIu32 ",%" PRIu32 ",%d,%d],\"nicb\":%d", m.cpr.lat, m.cpr.lon, m.cpr.odd ? 1 : 0,
                     m.cpr_surface ? 1 : 0, m.nic_supplement_b ? 1 : 0);
      if (m.has(adsb::modes::kFieldCpr) || (m.df >= 17 && m.df <= 18 && m.tc >= 5 && m.tc <= 22 && m.tc != 19))
        std::fprintf(out, ",\"nuc\":%u", static_cast<unsigned>(adsb::modes::position_nuc(m.tc)));
      if (m.has(adsb::modes::kFieldGroundSpeed)) std::fprintf(out, ",\"gs\":%.4f", static_cast<double>(m.ground_speed_kt));
      if (m.has(adsb::modes::kFieldTrack)) std::fprintf(out, ",\"trk\":%.4f", static_cast<double>(m.track_deg));
      if (m.has(adsb::modes::kFieldAirspeed))
        std::fprintf(out, ",\"%s\":%.2f", m.airspeed_true ? "tas" : "ias", static_cast<double>(m.airspeed_kt));
      if (m.has(adsb::modes::kFieldHeading)) std::fprintf(out, ",\"hdg\":%.4f", static_cast<double>(m.heading_deg));
      if (m.has(adsb::modes::kFieldVerticalRate))
        std::fprintf(out, ",\"vr\":%" PRId32 ",\"vr_gnss\":%d", m.vertical_rate_fpm, m.vertical_rate_gnss ? 1 : 0);
      if (m.has(adsb::modes::kFieldGnssBaroDiff)) std::fprintf(out, ",\"gnss_diff\":%" PRId32, m.gnss_baro_diff_ft);
      if (m.has(adsb::modes::kFieldCallsign)) put_callsign(out, "cs", m.callsign);
      if (m.has(adsb::modes::kFieldCategory)) std::fprintf(out, ",\"cat\":\"%02X\"", static_cast<unsigned>(m.category));
      if (m.has(adsb::modes::kFieldSquawk)) std::fprintf(out, ",\"sq\":\"%04X\"", static_cast<unsigned>(m.squawk));
      if (m.has(adsb::modes::kFieldEmergency)) std::fprintf(out, ",\"emerg\":%u", static_cast<unsigned>(m.emergency));
      if (m.has(adsb::modes::kFieldAirGround)) std::fprintf(out, ",\"ground\":%d", m.on_ground ? 1 : 0);
      if (m.has(adsb::modes::kFieldTargetState)) put_target(out, m.target);
      if (m.has(adsb::modes::kFieldOpStatus)) put_op_status(out, m.op_status);
      if (m.has(adsb::modes::kFieldCommB)) put_comm_b(out, m.comm_b);
    }

    // The engine, for positions only.
    adsb::state::FrameMeta meta{r.rx_ns, r.radio_ts, r.line, adsb::state::payload_hash(r.payload, r.len)};
    if (m.corrected_bit >= 0) {
      std::uint8_t fixed[14];
      std::memcpy(fixed, r.payload, r.len);
      fixed[m.corrected_bit / 8] = static_cast<std::uint8_t>(fixed[m.corrected_bit / 8] ^ (0x80u >> (m.corrected_bit % 8)));
      meta.payload_hash = adsb::state::payload_hash(fixed, r.len);
    }
    const std::uint64_t global0 = engine->stats().cpr_global;
    const std::uint64_t local0 = engine->stats().cpr_local;
    engine->on_decoded(st, m, meta, sink);
    const bool global = engine->stats().cpr_global != global0;
    if (global || engine->stats().cpr_local != local0) {
      const auto t = engine->store().find(m.icao);
      if (t) {
        std::fprintf(out, ",\"pos\":[%.7f,%.7f,\"%s\"]", t.hot->lat_e7 / 1e7, t.hot->lon_e7 / 1e7,
                     global ? "global" : "local");
      }
    }
    std::fputs("}\n", out);
  }
  if (std::fflush(out) != 0 || std::ferror(out) || (out != stdout && std::fclose(out) != 0)) {
    std::fprintf(stderr, "adsb-decode: write failed\n");
    return 1;
  }
  std::fprintf(stderr, "adsb-decode: %" PRIu64 " records\n", i);
  return 0;
}
