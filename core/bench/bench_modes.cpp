#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "adsb/ingest/beast.hpp"
#include "adsb/modes/cpr.hpp"
#include "adsb/modes/crc24.hpp"
#include "adsb/modes/decoder.hpp"

namespace {

using namespace adsb::modes;
using namespace adsb::ingest;

// A realistic mix of DF17 frames: identification, positions, velocities.
const std::vector<std::vector<std::uint8_t>>& sample_frames() {
  static const std::vector<std::vector<std::uint8_t>> frames = [] {
    const char* hexes[] = {
        "8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7",
        "8D40621D58C386435CC412692AD6", "8D485020994409940838175B284F",
        "8DA05F219B06B6AF189400CBC33F", "8D40058B58C901375147EFD09357",
        "8D40058B58C904A87F402D3B8C59", "8D406B902015A678D4D220AA4BDA",
    };
    std::vector<std::vector<std::uint8_t>> out;
    for (const char* h : hexes) {
      std::vector<std::uint8_t> f;
      for (int i = 0; i < 28; i += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
        f.push_back(static_cast<std::uint8_t>((nib(h[i]) << 4) | nib(h[i + 1])));
      }
      out.push_back(f);
    }
    return out;
  }();
  return frames;
}

void BM_Crc24Syndrome(benchmark::State& state) {
  const auto& frames = sample_frames();
  std::size_t i = 0;
  for (auto _ : state) {
    const auto& f = frames[i++ & 7];
    benchmark::DoNotOptimize(crc24_syndrome(f.data(), f.size()));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Crc24Syndrome);

void BM_DecodeDf17(benchmark::State& state) {
  const auto& frames = sample_frames();
  DecodedMsg m;
  std::size_t i = 0;
  for (auto _ : state) {
    const auto& f = frames[i++ & 7];
    benchmark::DoNotOptimize(decode(f.data(), f.size(), m));
    benchmark::DoNotOptimize(m);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeDf17);

void BM_CprGlobalAirborne(benchmark::State& state) {
  const CprFrame e{93000, 51372, false};
  const CprFrame o{74158, 50194, true};
  for (auto _ : state) {
    benchmark::DoNotOptimize(cpr_global_airborne(e, o, false));
  }
}
BENCHMARK(BM_CprGlobalAirborne);

void BM_CprLocalAirborne(benchmark::State& state) {
  const CprFrame e{93000, 51372, false};
  const LatLon ref{52.258, 3.918};
  for (auto _ : state) {
    benchmark::DoNotOptimize(cpr_local_airborne(e, ref));
  }
}
BENCHMARK(BM_CprLocalAirborne);

void BM_BeastParse(benchmark::State& state) {
  // ~1 MB of encoded long frames, parsed in 4 KB chunks like recv() would.
  std::vector<std::uint8_t> wire;
  const auto& frames = sample_frames();
  std::uint64_t ts = 0;
  while (wire.size() < (1u << 20)) {
    for (const auto& f : frames) {
      BeastFrame bf;
      bf.type = BeastType::kModeSLong;
      bf.len = 14;
      bf.timestamp = ts++ & 0xFFFFFFFFFFFFULL;
      bf.signal = 0x50;
      std::copy(f.begin(), f.end(), bf.payload);
      std::uint8_t buf[kBeastMaxEncodedLen];
      const std::size_t n = beast_encode(bf, buf);
      wire.insert(wire.end(), buf, buf + n);
    }
  }
  std::uint64_t frames_seen = 0;
  for (auto _ : state) {
    BeastParser p;
    for (std::size_t off = 0; off < wire.size(); off += 4096) {
      const std::size_t n = std::min<std::size_t>(4096, wire.size() - off);
      frames_seen += p.feed(wire.data() + off, n, [](const BeastFrame& f) {
        std::uint8_t first = f.payload[0];
        benchmark::DoNotOptimize(first);
      });
    }
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(static_cast<std::uint64_t>(state.iterations()) * wire.size()));
  state.SetItemsProcessed(static_cast<std::int64_t>(frames_seen));
}
BENCHMARK(BM_BeastParse)->Unit(benchmark::kMicrosecond);

}  // namespace
