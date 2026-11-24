// A/B arbiter cost per frame (README_UPDATES.md §6.2: "arbitration adds only
// a table lookup"). Two simulated lines, offered in arrival order; the
// recording's time is compressed by Arg(0) to put more frames inside each
// window (1 = real traffic, ~2,400 frames/s for 300 aircraft on two lines;
// 1000 = ~2.4 M frames/s, ~12,000 frames inside a 5 ms window).

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

#include "adsb/ingest/arbiter.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb;

const std::vector<record::Record>& two_lines() {
  static const std::vector<record::Record> recs = [] {
    sim::SynthConfig cfg;
    cfg.aircraft = 300;
    cfg.duration_s = 30;
    cfg.lines = 2;
    return sim::generate(cfg).records;
  }();
  return recs;
}

void BM_ArbiterOffer(benchmark::State& state) {
  const auto& base = two_lines();
  const auto squeeze = static_cast<std::uint64_t>(state.range(0));
  std::vector<record::Record> recs = base;
  const std::uint64_t t0 = recs.front().rx_ns;
  for (auto& r : recs) r.rx_ns = t0 + (r.rx_ns - t0) / squeeze;
  const std::uint64_t span = recs.back().rx_ns - t0 + 1;
  ingest::ArbiterConfig cfg;
  cfg.max_entries = 65536;
  auto arb = std::make_unique<ingest::Arbiter>(cfg);
  std::size_t i = 0;
  std::uint64_t shift = 0, forwarded = 0;
  for (auto _ : state) {
    record::Record r = recs[i];
    r.rx_ns += shift;
    forwarded += arb->offer(r) ? 1u : 0u;
    if (++i == recs.size()) {
      i = 0;
      shift += span + cfg.window_ns + 1;
    }
  }
  benchmark::DoNotOptimize(forwarded);
  state.counters["open"] = static_cast<double>(arb->open_entries());
  state.counters["evicted"] = static_cast<double>(arb->stats().evicted_early);
}
BENCHMARK(BM_ArbiterOffer)->Arg(1)->Arg(100)->Arg(1000);

}  // namespace
