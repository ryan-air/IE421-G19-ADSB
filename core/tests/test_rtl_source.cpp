// Tests for the live RTL-SDR source (ingest/rtl_device.hpp and the device
// path of IqSource), with the test device FakeRtlDevice in place of a
// dongle: it plays a cu8 file in USB-sized transfers from the thread that
// calls read_async, like librtlsdr, and fails on request.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "adsb/common/clock.hpp"
#include "adsb/demod/demod.hpp"
#include "adsb/ingest/iq_source.hpp"
#include "adsb/ingest/rtl_device.hpp"
#include "adsb/sim/iq.hpp"
#include "adsb/sim/synth.hpp"

namespace {

using namespace adsb;
using adsb::ingest::FakeRtlDevice;
using adsb::ingest::FakeRtlOptions;
using adsb::ingest::IqSource;
using adsb::ingest::IqSourceConfig;
using adsb::ingest::RtlConfig;
using adsb::ingest::RtlDevice;
using adsb::ingest::RtlGain;

std::vector<std::uint8_t> render(int aircraft, double seconds, std::uint64_t seed) {
  sim::SynthConfig syn;
  syn.seed = seed;
  syn.aircraft = aircraft;
  syn.duration_s = seconds;
  syn.keep_transmissions = true;
  const auto traffic = sim::generate(syn);
  sim::IqConfig cfg;
  cfg.seed = seed;
  const sim::IqPlan plan = sim::plan_iq(syn, traffic, cfg);
  std::vector<std::uint8_t> cu8(plan.total_samples * 2);
  std::vector<float> chunk(2 * sim::kIqChunkSamples);
  for (std::uint64_t c = 0; c * sim::kIqChunkSamples < plan.total_samples; ++c) {
    const std::size_t n = sim::render_iq_chunk(plan, cfg, c, chunk.data());
    sim::quantize_iq(chunk.data(), n, sim::IqFormat::kCu8, cu8.data() + c * sim::kIqChunkSamples * 2);
  }
  return cu8;
}

std::string temp_file(const std::string& name, const std::vector<std::uint8_t>& bytes) {
  const std::string path = ::testing::TempDir() + "adsb_rtl_" + name + "_" + std::to_string(::getpid());
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

// Real-time multiple this build's demodulator manages on these samples
// (debug and sanitizer builds are many times slower than release ones).
double demod_speed(const std::vector<std::uint8_t>& cu8) {
  const std::uint64_t t0 = mono_ns();
  (void)direct(cu8, 8192);
  return static_cast<double>(cu8.size() / 2) / 2.4e6 / (static_cast<double>(mono_ns() - t0) * 1e-9);
}

FakeRtlOptions fake_opts(const std::string& path) {
  FakeRtlOptions o;
  o.path = path;
  return o;
}

IqSourceConfig device_config(std::size_t block = 16384, std::size_t buffers = 16) {
  IqSourceConfig cfg;
  cfg.path = "fake";
  cfg.block_samples = block;
  cfg.buffers = buffers;
  return cfg;
}

struct Outcome {
  std::vector<record::Record> recs;
  ingest::IqSourceStats stats;
  std::string error;
  std::vector<std::string> status;
};

// Opens src (ASSERTs) and runs it to the end.
template <typename Sink>
void run_source(IqSource& src, Outcome& out, std::atomic<bool>& stop, Sink&& extra) {
  std::string err;
  ASSERT_TRUE(src.open(&err)) << err;
  src.run(
      stop,
      [&](const record::Record& r) {
        out.recs.push_back(r);
        extra(r);
      },
      [&](const char* s) { out.status.emplace_back(s); });
  out.stats = src.stats();
  out.error = src.error_text();
}

// ---------------------------------------------------------------------------
// Parsing and configuration
// ---------------------------------------------------------------------------

TEST(RtlDevice, ParsesDevicePathsAndGains) {
  std::string dev;
  EXPECT_TRUE(ingest::parse_rtlsdr_path("rtlsdr", &dev));
  EXPECT_EQ(dev, "0");
  EXPECT_TRUE(ingest::parse_rtlsdr_path("rtlsdr:2", &dev));
  EXPECT_EQ(dev, "2");
  EXPECT_TRUE(ingest::parse_rtlsdr_path("rtlsdr:00001090", &dev));
  EXPECT_EQ(dev, "00001090");
  EXPECT_FALSE(ingest::parse_rtlsdr_path("rtlsdr:", &dev));
  EXPECT_FALSE(ingest::parse_rtlsdr_path("rtlsdrx", &dev));
  EXPECT_FALSE(ingest::parse_rtlsdr_path("capture.cu8", &dev));
  EXPECT_FALSE(ingest::parse_rtlsdr_path("-", &dev));

  RtlGain g{};
  double db = -1;
  EXPECT_TRUE(ingest::parse_rtl_gain("max", g, db));
  EXPECT_EQ(g, RtlGain::kMax);
  EXPECT_TRUE(ingest::parse_rtl_gain("auto", g, db));
  EXPECT_EQ(g, RtlGain::kAuto);
  EXPECT_TRUE(ingest::parse_rtl_gain("40.2", g, db));
  EXPECT_EQ(g, RtlGain::kManual);
  EXPECT_DOUBLE_EQ(db, 40.2);
  EXPECT_FALSE(ingest::parse_rtl_gain("", g, db));
  EXPECT_FALSE(ingest::parse_rtl_gain("loud", g, db));
  EXPECT_FALSE(ingest::parse_rtl_gain("40dB", g, db));
  EXPECT_FALSE(ingest::parse_rtl_gain("1000", g, db));

  const std::vector<int> steps = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254,
                                  280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
  EXPECT_EQ(ingest::nearest_gain(steps, 40.0), 402);
  EXPECT_EQ(ingest::nearest_gain(steps, 49.6), 496);
  EXPECT_EQ(ingest::nearest_gain(steps, 99.0), 496);
  EXPECT_EQ(ingest::nearest_gain(steps, -5.0), 0);
  EXPECT_EQ(ingest::nearest_gain(steps, 0.45), 0);    // 0 and 9 tie at 4.5: the lower one
  EXPECT_EQ(ingest::nearest_gain(steps, 0.46), 9);
  EXPECT_EQ(ingest::nearest_gain({}, 12.3), 123);

  EXPECT_TRUE(ingest::rtl_rate_ok(2.4e6));
  EXPECT_TRUE(ingest::rtl_rate_ok(3.2e6));
  EXPECT_TRUE(ingest::rtl_rate_ok(250e3));
  EXPECT_FALSE(ingest::rtl_rate_ok(225e3));
  EXPECT_FALSE(ingest::rtl_rate_ok(900e3));
  EXPECT_FALSE(ingest::rtl_rate_ok(500e3));
  EXPECT_FALSE(ingest::rtl_rate_ok(3.3e6));
}

TEST(RtlDevice, ConfigureAppliesTheSettingsAndReadsThemBack) {
  const std::string path = temp_file("cfg", std::vector<std::uint8_t>(4096, 127));
  auto configured = [&](const RtlConfig& rc, double rate, ingest::RtlInfo* info, std::string* err) {
    auto dev = std::make_unique<FakeRtlDevice>(fake_opts(path));
    EXPECT_TRUE(dev->open("0", err));
    const bool ok = ingest::configure_rtl(*dev, rc, rate, info, err);
    return std::make_pair(ok, std::move(dev));
  };
  ingest::RtlInfo info;
  std::string err;
  {
    RtlConfig rc;  // defaults: max gain, 1090 MHz
    auto [ok, dev] = configured(rc, 2.4e6, &info, &err);
    ASSERT_TRUE(ok) << err;
    EXPECT_TRUE(dev->manual());
    EXPECT_EQ(dev->tuner_gain(), 496);
    EXPECT_EQ(dev->center_freq(), 1'090'000'000u);
    EXPECT_EQ(dev->sample_rate(), 2'400'000u);
    EXPECT_EQ(dev->ppm(), 0);
    EXPECT_FALSE(dev->agc());
    EXPECT_FALSE(dev->bias_tee());
    EXPECT_EQ(dev->resets(), 1);
    EXPECT_DOUBLE_EQ(info.gain_db, 49.6);
    EXPECT_FALSE(info.gain_auto);
    EXPECT_EQ(info.tuner, "R820T");
    EXPECT_EQ(info.sample_rate_hz, 2.4e6);
    EXPECT_EQ(info.freq_hz, 1090e6);
    EXPECT_NE(ingest::rtl_info_text(info).find("49.6 dB"), std::string::npos) << ingest::rtl_info_text(info);
  }
  {
    RtlConfig rc;
    rc.gain = RtlGain::kManual;
    rc.gain_db = 40;
    rc.ppm = -12;
    rc.agc = true;
    rc.bias_tee = true;
    rc.freq_hz = 1089.5e6;
    auto [ok, dev] = configured(rc, 2.0e6, &info, &err);
    ASSERT_TRUE(ok) << err;
    EXPECT_TRUE(dev->manual());
    EXPECT_EQ(dev->tuner_gain(), 402);
    EXPECT_EQ(dev->ppm(), -12);
    EXPECT_TRUE(dev->agc());
    EXPECT_TRUE(dev->bias_tee());
    EXPECT_EQ(dev->center_freq(), 1'089'500'000u);
    EXPECT_DOUBLE_EQ(info.gain_db, 40.2);
    EXPECT_EQ(info.ppm, -12);
  }
  {
    RtlConfig rc;
    rc.gain = RtlGain::kAuto;
    auto [ok, dev] = configured(rc, 2.4e6, &info, &err);
    ASSERT_TRUE(ok) << err;
    EXPECT_FALSE(dev->manual());
    EXPECT_TRUE(info.gain_auto);
    EXPECT_NE(ingest::rtl_info_text(info).find("tuner AGC"), std::string::npos);
  }
  {
    RtlConfig rc;
    auto [ok, dev] = configured(rc, 1.0e5, &info, &err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("sample rate"), std::string::npos) << err;
  }
  {
    RtlConfig rc;
    rc.freq_hz = 5e9;
    auto [ok, dev] = configured(rc, 2.4e6, &info, &err);
    EXPECT_FALSE(ok);
  }
  std::remove(path.c_str());
}

// Without librtlsdr in the build, `rtlsdr` says so; with it (and, as in CI,
// no dongle attached) opening fails with the library's answer. Either way
// nothing hangs.
TEST(RtlSource, ARealDeviceThatIsNotThereFailsToOpen) {
  IqSourceConfig cfg;
  cfg.path = "rtlsdr:0";
  IqSource src(cfg);
  std::string err;
  if (!ingest::rtlsdr_built()) {
    EXPECT_FALSE(src.open(&err));
    EXPECT_NE(err.find("built without librtlsdr"), std::string::npos) << err;
    EXPECT_EQ(ingest::make_librtlsdr_device(), nullptr);
  } else if (src.open(&err)) {
    GTEST_SKIP() << "an RTL-SDR is attached";
  } else {
    EXPECT_FALSE(err.empty());
  }
}

TEST(RtlSource, RejectsTransfersThatAreNotWhole512ByteUnitsAndOtherFormats) {
  std::string why;
  IqSourceConfig cfg;
  cfg.path = "rtlsdr";
  EXPECT_TRUE(ingest::validate(cfg, &why)) << why;
  cfg.block_samples = 1000;  // 2000 bytes
  EXPECT_FALSE(ingest::validate(cfg, &why));
  EXPECT_NE(why.find("512"), std::string::npos) << why;
  cfg.block_samples = 256;  // 512 bytes
  EXPECT_TRUE(ingest::validate(cfg, &why)) << why;
  cfg.block_samples = 16384;
  cfg.format = demod::SampleFormat::kCs16;
  EXPECT_FALSE(ingest::validate(cfg, &why));
  cfg.format = demod::SampleFormat::kCu8;
  cfg.sample_rate_hz = 6e6;
  EXPECT_FALSE(ingest::validate(cfg, &why));
  // A file path is not held to the device's rules.
  IqSourceConfig file;
  file.path = "capture.cu8";
  file.block_samples = 1000;
  EXPECT_TRUE(ingest::validate(file, &why)) << why;

  // The same checks for a device handed over under another name.
  const std::string path = temp_file("rej", std::vector<std::uint8_t>(4096, 127));
  IqSourceConfig odd = device_config(1000);
  IqSource src(odd);
  src.use_device(std::make_unique<FakeRtlDevice>(fake_opts(path)));
  EXPECT_FALSE(src.open(&why));
  EXPECT_NE(why.find("512"), std::string::npos) << why;
  IqSource refused(device_config());
  FakeRtlOptions fo{path};
  fo.fail_open = true;
  refused.use_device(std::make_unique<FakeRtlDevice>(fo));
  EXPECT_FALSE(refused.open(&why));
  EXPECT_NE(why.find("refused"), std::string::npos) << why;
  std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

// Paced at most a third of this build's demodulator speed: no transfer is
// dropped, and the frames are exactly adsb-demod's on the same samples, in
// transfers of the block size. Frame times follow arrival.
TEST(RtlSource, AFakeDeviceGivesExactlyTheDemodulatorsFrames) {
  const auto cu8 = render(60, 1.5, 71);
  const std::string path = temp_file("same", cu8);
  const double speed = std::min(3.0, demod_speed(cu8) / 3);
  for (std::size_t block : {std::size_t{4096}, std::size_t{16384}}) {
    IqSourceConfig cfg = device_config(block, 256);
    IqSource src(cfg);
    FakeRtlOptions fo{path};
    fo.speed = speed;
    auto dev = std::make_unique<FakeRtlDevice>(fo);
    FakeRtlDevice* fake = dev.get();
    src.use_device(std::move(dev));
    Outcome run;
    std::atomic<bool> stop{false};
    std::vector<std::uint64_t> seen;
    run_source(src, run, stop, [&](const record::Record&) { seen.push_back(mono_ns()); });
    EXPECT_TRUE(run.error.empty()) << run.error;
    EXPECT_FALSE(run.stats.read_error);
    EXPECT_TRUE(run.stats.device);
    EXPECT_EQ(fake->last_buf_len(), block * 2);
    EXPECT_EQ(fake->closes(), 1);
    EXPECT_FALSE(fake->is_open());
    EXPECT_EQ(run.stats.samples, cu8.size() / 2);
    ASSERT_FALSE(run.status.empty());
    EXPECT_EQ(run.status.back(), "end of input");
    const auto ref = direct(cu8, block);
    ASSERT_GT(ref.size(), 200u);
    if (run.stats.blocks_dropped == 0) {
      ASSERT_EQ(run.recs.size(), ref.size()) << block;
      for (std::size_t i = 0; i < ref.size(); ++i) {
        EXPECT_EQ(hex(run.recs[i].payload, run.recs[i].len), hex(ref[i].bytes, ref[i].len));
        EXPECT_EQ(run.recs[i].radio_ts, static_cast<std::uint64_t>(std::llround(ref[i].sample_pos * 5.0)));
      }
    } else {
      // A stalled runner: whatever was decoded is still adsb-demod's.
      EXPECT_LE(run.stats.blocks_dropped * 20, run.stats.blocks);
      std::size_t j = 0;
      for (const auto& r : run.recs) {
        while (j < ref.size() && hex(ref[j].bytes, ref[j].len) != hex(r.payload, r.len)) ++j;
        EXPECT_LT(j, ref.size());
      }
    }
    // Times by arrival: never after the sink saw the frame, the median wait
    // about a block plus processing, and spaced like the samples / speed.
    std::vector<std::uint64_t> delay;
    for (std::size_t i = 0; i < run.recs.size(); ++i) {
      EXPECT_LE(run.recs[i].rx_ns, seen[i] + 1000);
      if (i) {
        EXPECT_GE(run.recs[i].rx_ns, run.recs[i - 1].rx_ns);
      }
      delay.push_back(seen[i] - std::min(seen[i], run.recs[i].rx_ns));
    }
    std::nth_element(delay.begin(), delay.begin() + static_cast<std::ptrdiff_t>(delay.size() / 2), delay.end());
    EXPECT_LT(delay[delay.size() / 2], 50'000'000u);
    const double dt_rx = static_cast<double>(run.recs.back().rx_ns - run.recs.front().rx_ns);
    const double dt_samples =
        static_cast<double>(run.recs.back().radio_ts - run.recs.front().radio_ts) / 12e6 / speed * 1e9;
    EXPECT_NEAR(dt_rx, dt_samples, 5e6 + 0.01 * dt_samples);
  }
  std::remove(path.c_str());
}

// A consumer slower than the device: the callback never waits; whole
// transfers are dropped and counted, the demodulator hears of each gap, and
// frame times stay monotonic.
TEST(RtlSource, ASlowConsumerDropsWholeTransfersAndCountsThem) {
  const auto cu8 = render(60, 2.0, 72);
  const std::string path = temp_file("slow", cu8);
  IqSource src(device_config(8192, 2));
  FakeRtlOptions fo{path};
  fo.speed = 20;
  src.use_device(std::make_unique<FakeRtlDevice>(fo));
  Outcome run;
  std::atomic<bool> stop{false};
  run_source(src, run, stop, [](const record::Record&) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
  const auto& st = run.stats;
  EXPECT_TRUE(run.error.empty()) << run.error;
  EXPECT_GT(st.blocks_dropped, 10u);
  EXPECT_EQ(st.samples, cu8.size() / 2);
  EXPECT_LE(st.samples_dropped, st.blocks_dropped * 8192);
  EXPECT_GT(st.samples_dropped, (st.blocks_dropped - 1) * 8192);
  EXPECT_EQ(st.demod.samples + st.samples_dropped, st.samples);
  EXPECT_GT(st.demod.skipped, 0u);
  EXPECT_LE(st.demod.skipped, st.samples_dropped);
  EXPECT_LE(st.ring_high_water, 2u);
  ASSERT_GT(run.recs.size(), 0u);
  for (std::size_t i = 1; i < run.recs.size(); ++i) {
    EXPECT_GE(run.recs[i].rx_ns, run.recs[i - 1].rx_ns);
  }
  std::remove(path.c_str());
}

// A late first transfer (USB start-up) must not offset every later frame:
// frames are timed on the lower envelope of the arrivals, not anchored on
// the first block.
TEST(RtlSource, ALateFirstTransferDoesNotShiftLaterFrameTimes) {
  const auto cu8 = render(60, 1.5, 79);
  const std::string path = temp_file("late", cu8);
  const double speed = std::min(2.0, demod_speed(cu8) / 3);
  IqSource src(device_config(4096, 256));
  FakeRtlOptions fo = fake_opts(path);
  fo.speed = speed;
  fo.hold_first_ns = 150'000'000;
  src.use_device(std::make_unique<FakeRtlDevice>(fo));
  Outcome run;
  std::atomic<bool> stop{false};
  std::vector<std::uint64_t> seen;
  run_source(src, run, stop, [&](const record::Record&) { seen.push_back(mono_ns()); });
  EXPECT_TRUE(run.error.empty()) << run.error;
  ASSERT_GT(run.recs.size(), 200u);
  // The second half: never later than when the sink saw the frame, and the
  // median wait well under the 150 ms hold.
  std::vector<std::uint64_t> delay;
  std::size_t future = 0;
  for (std::size_t i = run.recs.size() / 2; i < run.recs.size(); ++i) {
    if (run.recs[i].rx_ns > seen[i] + 1000) ++future;
    delay.push_back(seen[i] - std::min(seen[i], run.recs[i].rx_ns));
  }
  EXPECT_EQ(future, 0u);
  std::nth_element(delay.begin(), delay.begin() + static_cast<std::ptrdiff_t>(delay.size() / 2), delay.end());
  EXPECT_LT(delay[delay.size() / 2], 50'000'000u);
  std::remove(path.c_str());
}

// Transfers of any (even) length are cut into ring blocks; the frames do not
// depend on how the samples arrived.
class ChoppyDevice final : public RtlDevice {
 public:
  explicit ChoppyDevice(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  bool open(const std::string&, std::string*) override { return true; }
  void close() noexcept override {}
  std::string name() const override { return "choppy"; }
  std::string serial() const override { return ""; }
  std::string tuner() const override { return "none"; }
  int set_sample_rate(std::uint32_t hz) override { rate_ = hz; return 0; }
  std::uint32_t sample_rate() const override { return rate_; }
  int set_center_freq(std::uint32_t hz) override { freq_ = hz; return 0; }
  std::uint32_t center_freq() const override { return freq_; }
  int set_freq_correction(int) override { return 0; }
  std::vector<int> tuner_gains() const override { return {0, 496}; }
  int set_tuner_gain_mode(bool) override { return 0; }
  int set_tuner_gain(int) override { return 0; }
  int tuner_gain() const override { return 496; }
  int set_agc_mode(bool) override { return 0; }
  int set_bias_tee(bool) override { return 0; }
  int reset_buffer() override { return 0; }
  int read_async(Callback cb, void* ctx, std::uint32_t, std::uint32_t buf_len) override {
    // 2.5 blocks, then 2 bytes, then one block minus 2 bytes, ...
    const std::size_t sizes[] = {buf_len * 5 / 2, 2, buf_len - 2, buf_len, 3 * buf_len + 512};
    std::size_t off = 0, k = 0;
    while (off < bytes_.size()) {
      const std::size_t n = std::min(sizes[k++ % 5], bytes_.size() - off);
      cb(bytes_.data() + off, static_cast<std::uint32_t>(n), ctx);
      off += n;
      // Slow enough for the consumer of a debug build to keep up.
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return kEndOfInput;
  }
  int cancel_async() override { return 0; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::uint32_t rate_ = 0, freq_ = 0;
};

TEST(RtlSource, TransfersOfAnyLengthAreCutIntoBlocks) {
  const auto cu8 = render(40, 1.0, 73);
  IqSource src(device_config(4096, 4096));
  src.use_device(std::make_unique<ChoppyDevice>(cu8));
  Outcome run;
  std::atomic<bool> stop{false};
  run_source(src, run, stop, [](const record::Record&) {});
  EXPECT_TRUE(run.error.empty()) << run.error;
  ASSERT_EQ(run.stats.blocks_dropped, 0u);
  EXPECT_EQ(run.stats.samples, cu8.size() / 2);
  const auto ref = direct(cu8, 4096);
  ASSERT_EQ(run.recs.size(), ref.size());
  for (std::size_t i = 0; i < ref.size(); ++i) {
    EXPECT_EQ(hex(run.recs[i].payload, run.recs[i].len), hex(ref[i].bytes, ref[i].len));
    EXPECT_EQ(run.recs[i].radio_ts, static_cast<std::uint64_t>(std::llround(ref[i].sample_pos * 5.0)));
  }
}

// Stop (Ctrl-C) cancels the transfers: the run ends promptly, without an
// error, and the device is closed. Median of three, for loaded machines.
TEST(RtlSource, StopCancelsTheTransfersPromptly) {
  const auto cu8 = render(20, 3.0, 74);
  const std::string path = temp_file("stop", cu8);
  std::vector<std::uint64_t> took;
  for (int trial = 0; trial < 3; ++trial) {
    IqSource src(device_config());
    FakeRtlOptions fo{path};
    fo.speed = 1;  // 3 s of samples
    auto dev = std::make_unique<FakeRtlDevice>(fo);
    FakeRtlDevice* fake = dev.get();
    src.use_device(std::move(dev));
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> stop_at{0};
    std::thread stopper([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      stop_at.store(mono_ns());
      stop.store(true);
    });
    Outcome run;
    run_source(src, run, stop, [](const record::Record&) {});
    const std::uint64_t end = mono_ns();
    stopper.join();
    took.push_back(end - stop_at.load());
    EXPECT_TRUE(run.error.empty()) << run.error;
    EXPECT_FALSE(run.stats.read_error);
    EXPECT_LT(run.stats.samples, cu8.size() / 2);
    EXPECT_GE(fake->cancel_calls(), 1u);
    EXPECT_EQ(fake->closes(), 1);
    ASSERT_FALSE(run.status.empty());
    EXPECT_EQ(run.status.back(), "stopped");
  }
  std::sort(took.begin(), took.end());
  EXPECT_LT(took[1], 100'000'000u) << took[0] << " " << took[1] << " " << took[2];
  std::remove(path.c_str());
}

// Stop before the first transfer: librtlsdr ignores a cancel that comes
// before streaming starts, so the consumer repeats it until the reader ends.
TEST(RtlSource, StopBeforeTheFirstTransferStillEnds) {
  const auto cu8 = render(5, 1.0, 75);
  const std::string path = temp_file("early", cu8);
  IqSource src(device_config());
  FakeRtlOptions fo{path};
  fo.speed = 0.01;  // the first transfer is due 0.7 s from now
  src.use_device(std::make_unique<FakeRtlDevice>(fo));
  std::atomic<bool> stop{false};
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
  });
  const std::uint64_t t0 = mono_ns();
  Outcome run;
  run_source(src, run, stop, [](const record::Record&) {});
  stopper.join();
  EXPECT_LT(mono_ns() - t0, 500'000'000u);
  EXPECT_EQ(run.stats.samples, 0u);
  EXPECT_TRUE(run.error.empty()) << run.error;
}

TEST(RtlSource, AUsbErrorEndsTheRunWithAnError) {
  const auto cu8 = render(30, 2.0, 76);
  const std::string path = temp_file("err", cu8);
  IqSource src(device_config(16384, 64));
  FakeRtlOptions fo{path};
  fo.speed = 4;
  fo.error_after_bytes = 10 * 32768;
  auto dev = std::make_unique<FakeRtlDevice>(fo);
  FakeRtlDevice* fake = dev.get();
  src.use_device(std::move(dev));
  Outcome run;
  std::atomic<bool> stop{false};
  run_source(src, run, stop, [](const record::Record&) {});
  EXPECT_TRUE(run.stats.read_error);
  EXPECT_NE(run.error.find("failed"), std::string::npos) << run.error;
  EXPECT_EQ(run.stats.samples, 10u * 16384);
  EXPECT_EQ(fake->closes(), 1);
  ASSERT_FALSE(run.status.empty());
  EXPECT_EQ(run.status.back().rfind("device error: ", 0), 0u) << run.status.back();
  std::remove(path.c_str());
}

// librtlsdr returns 0 by itself when the dongle is unplugged: not a stop.
TEST(RtlSource, ALostDeviceEndsTheRunWithAnError) {
  const auto cu8 = render(30, 2.0, 77);
  const std::string path = temp_file("lost", cu8);
  IqSource src(device_config(16384, 64));
  FakeRtlOptions fo{path};
  fo.speed = 4;
  fo.unplug_after_bytes = 5 * 32768;
  src.use_device(std::make_unique<FakeRtlDevice>(fo));
  Outcome run;
  std::atomic<bool> stop{false};
  run_source(src, run, stop, [](const record::Record&) {});
  EXPECT_TRUE(run.stats.read_error);
  EXPECT_NE(run.error.find("stopped delivering"), std::string::npos) << run.error;
  EXPECT_EQ(run.stats.samples, 5u * 16384);
  std::remove(path.c_str());
}

// A device that just stops calling back is given up after the stall timeout.
TEST(RtlSource, AStalledDeviceIsGivenUpAfterTheTimeout) {
  const auto cu8 = render(30, 2.0, 78);
  const std::string path = temp_file("stall", cu8);
  IqSourceConfig cfg = device_config(16384, 64);
  cfg.rtl.stall_timeout_ns = 200'000'000;
  IqSource src(cfg);
  FakeRtlOptions fo{path};
  fo.speed = 4;
  fo.stall_after_bytes = 5 * 32768;
  auto dev = std::make_unique<FakeRtlDevice>(fo);
  FakeRtlDevice* fake = dev.get();
  src.use_device(std::move(dev));
  Outcome run;
  std::atomic<bool> stop{false};
  const std::uint64_t t0 = mono_ns();
  run_source(src, run, stop, [](const record::Record&) {});
  const std::uint64_t took = mono_ns() - t0;
  EXPECT_TRUE(run.stats.read_error);
  EXPECT_NE(run.error.find("no samples from the device for 0.2 s"), std::string::npos) << run.error;
  EXPECT_EQ(run.stats.samples, 5u * 16384);
  EXPECT_GE(fake->cancel_calls(), 1u);
  EXPECT_GT(took, 200'000'000u);
  EXPECT_LT(took, 3'000'000'000u);
  std::remove(path.c_str());
}

}  // namespace
