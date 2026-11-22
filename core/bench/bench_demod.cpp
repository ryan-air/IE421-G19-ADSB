// Demodulator throughput (plan §5.2): magnitude methods on one block, and
// the whole demodulator on a synthetic scenario. items/s = samples/s; at
// 2.4 MS/s one real-time receiver needs 2.4 M items/s.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "adsb/demod/demod.hpp"
#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb;

// 2 s of 150 aircraft at 2.4 MS/s, as cu8 (the RTL-SDR format).
const std::vector<std::uint8_t>& scenario_cu8() {
  static const std::vector<std::uint8_t> data = [] {
    sim::SynthConfig syn;
    syn.seed = 11;
    syn.aircraft = 150;
    syn.duration_s = 2.0;
    syn.keep_transmissions = true;
    const sim::SynthOutput traffic = sim::generate(syn);
    sim::IqConfig cfg;
    cfg.seed = 11;
    const sim::IqPlan plan = sim::plan_iq(syn, traffic, cfg);
    std::vector<std::uint8_t> out(plan.total_samples * 2);
    std::vector<float> chunk(2 * sim::kIqChunkSamples);
    for (std::uint64_t c = 0; c * sim::kIqChunkSamples < plan.total_samples; ++c) {
      const std::size_t n = sim::render_iq_chunk(plan, cfg, c, chunk.data());
      sim::quantize_iq(chunk.data(), n, sim::IqFormat::kCu8, out.data() + c * sim::kIqChunkSamples * 2);
    }
    return out;
  }();
  return data;
}

void BM_Magnitude(benchmark::State& state) {
  const auto method = static_cast<demod::Magnitude>(state.range(0));
  const auto& data = scenario_cu8();
  constexpr std::size_t kN = 1 << 16;
  std::vector<float> out(kN);
  for (auto _ : state) {
    demod::magnitudes(data.data(), kN, demod::SampleFormat::kCu8, method, out.data());
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(kN));
  state.SetLabel(demod::magnitude_name(method));
}
BENCHMARK(BM_Magnitude)->Arg(0)->Arg(1)->Arg(2);

void BM_DemodScenario(benchmark::State& state) {
  const auto& data = scenario_cu8();
  const std::size_t total = data.size() / 2;
  demod::DemodConfig cfg;
  cfg.magnitude = static_cast<demod::Magnitude>(state.range(0));
  std::uint64_t frames = 0;
  for (auto _ : state) {
    demod::Demodulator d(cfg);
    for (std::size_t off = 0; off < total; off += cfg.block_samples) {
      const std::size_t n = std::min(cfg.block_samples, total - off);
      frames += d.feed(data.data() + off * 2, n, demod::SampleFormat::kCu8).size();
    }
    frames += d.flush().size();
  }
  benchmark::DoNotOptimize(frames);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(total));
  state.SetLabel(demod::magnitude_name(cfg.magnitude));
}
BENCHMARK(BM_DemodScenario)->Arg(0)->Arg(1)->Arg(2)->Unit(benchmark::kMillisecond);

}  // namespace
