#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "adsb/common/clock.hpp"
#include "adsb/common/histogram.hpp"

namespace {

void BM_HistogramRecord(benchmark::State& state) {
  auto h = std::make_unique<adsb::LatencyHistogram>();
  std::mt19937_64 rng(1);
  std::lognormal_distribution<double> dist(7.0, 1.0);
  std::vector<std::uint64_t> values(4096);
  for (auto& v : values) v = static_cast<std::uint64_t>(dist(rng));
  std::size_t i = 0;
  for (auto _ : state) {
    h->record(values[i++ & 4095]);
  }
  benchmark::DoNotOptimize(h->count());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HistogramRecord);

void BM_HistogramPercentile(benchmark::State& state) {
  auto h = std::make_unique<adsb::LatencyHistogram>();
  std::mt19937_64 rng(2);
  for (int i = 0; i < 1'000'000; ++i) h->record(rng() % 10'000'000);
  for (auto _ : state) {
    benchmark::DoNotOptimize(h->percentile(99.9));
  }
}
BENCHMARK(BM_HistogramPercentile);

void BM_Ticks(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(adsb::ticks());
}
BENCHMARK(BM_Ticks);

void BM_MonoNs(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(adsb::mono_ns());
}
BENCHMARK(BM_MonoNs);

}  // namespace
