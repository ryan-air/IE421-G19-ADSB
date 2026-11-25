#include "adsb/ingest/rtl_device.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "adsb/common/clock.hpp"

namespace adsb::ingest {

bool parse_rtlsdr_path(const std::string& path, std::string* device) {
  if (path == "rtlsdr") {
    if (device) *device = "0";
    return true;
  }
  if (path.rfind("rtlsdr:", 0) != 0 || path.size() == 7) return false;
  if (device) *device = path.substr(7);
  return true;
}

bool parse_rtl_gain(const std::string& s, RtlGain& mode, double& db) {
  if (s == "max") {
    mode = RtlGain::kMax;
    return true;
  }
  if (s == "auto") {
    mode = RtlGain::kAuto;
    return true;
  }
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (s.empty() || end == s.c_str() || *end != '\0' || !(v >= -10.0 && v <= 100.0)) return false;
  mode = RtlGain::kManual;
  db = v;
  return true;
}

int nearest_gain(const std::vector<int>& gains, double db) noexcept {
  if (gains.empty()) return static_cast<int>(std::lround(db * 10.0));
  const double want = db * 10.0;
  int best = gains.front();
  for (const int g : gains) {
    if (std::fabs(static_cast<double>(g) - want) < std::fabs(static_cast<double>(best) - want)) best = g;
  }
  return best;
}

bool rtl_rate_ok(double hz) noexcept {
  return (hz > 225'000 && hz <= 300'000) || (hz > 900'000 && hz <= 3'200'000);
}

bool configure_rtl(RtlDevice& dev, const RtlConfig& cfg, double rate, RtlInfo* info, std::string* error) {
  auto fail = [&](const std::string& m, int r) {
    if (error) *error = m + " (error " + std::to_string(r) + ")";
    return false;
  };
  if (!rtl_rate_ok(rate)) {
    if (error) *error = "sample rate " + std::to_string(rate) + " Hz is outside the RTL2832's 225-300 kHz and 0.9-3.2 MHz";
    return false;
  }
  if (!(cfg.freq_hz >= 24e6 && cfg.freq_hz <= 1.8e9)) {
    if (error) *error = "centre frequency must be in 24 MHz - 1.8 GHz";
    return false;
  }
  int r = dev.set_sample_rate(static_cast<std::uint32_t>(std::lround(rate)));
  if (r < 0) return fail("cannot set the sample rate", r);
  r = dev.set_center_freq(static_cast<std::uint32_t>(std::llround(cfg.freq_hz)));
  if (r < 0) return fail("cannot set the centre frequency", r);
  r = dev.set_freq_correction(cfg.ppm);
  if (r < 0 && r != -2) return fail("cannot set the frequency correction", r);  // -2: already that value
  int gain = 0;
  if (cfg.gain == RtlGain::kAuto) {
    r = dev.set_tuner_gain_mode(false);
    if (r < 0) return fail("cannot enable tuner AGC", r);
  } else {
    const std::vector<int> gains = dev.tuner_gains();
    if (cfg.gain == RtlGain::kMax) {
      gain = gains.empty() ? 496 : gains.back();
    } else {
      gain = nearest_gain(gains, cfg.gain_db);
    }
    r = dev.set_tuner_gain_mode(true);
    if (r < 0) return fail("cannot set manual gain mode", r);
    r = dev.set_tuner_gain(gain);
    if (r < 0) return fail("cannot set the tuner gain", r);
  }
  r = dev.set_agc_mode(cfg.agc);
  if (r < 0) return fail("cannot set the RTL2832 AGC", r);
  // Only switched when asked: older librtlsdr builds and dongles without a
  // bias tee refuse it.
  if (cfg.bias_tee) {
    r = dev.set_bias_tee(true);
    if (r < 0) return fail("cannot switch the bias tee on", r);
  }
  r = dev.reset_buffer();
  if (r < 0) return fail("cannot reset the USB buffer", r);
  if (info) {
    info->name = dev.name();
    info->serial = dev.serial();
    info->tuner = dev.tuner();
    info->sample_rate_hz = dev.sample_rate();
    info->freq_hz = dev.center_freq();
    info->gain_auto = cfg.gain == RtlGain::kAuto;
    info->gain_db = info->gain_auto ? 0.0 : dev.tuner_gain() / 10.0;
    info->ppm = cfg.ppm;
    info->agc = cfg.agc;
    info->bias_tee = cfg.bias_tee;
  }
  return true;
}

std::string rtl_info_text(const RtlInfo& i) {
  char buf[512];
  char gain[48];
  if (i.gain_auto) {
    std::snprintf(gain, sizeof gain, "tuner AGC");
  } else {
    std::snprintf(gain, sizeof gain, "%.1f dB", i.gain_db);
  }
  std::snprintf(buf, sizeof buf, "%s (serial %s), tuner %s, %.3f MS/s, %.3f MHz, gain %s, %d ppm, RTL AGC %s, bias tee %s",
                i.name.c_str(), i.serial.empty() ? "-" : i.serial.c_str(), i.tuner.c_str(), i.sample_rate_hz / 1e6,
                i.freq_hz / 1e6, gain, i.ppm, i.agc ? "on" : "off", i.bias_tee ? "on" : "off");
  return buf;
}

// ---------------------------------------------------------------------------
// FakeRtlDevice
// ---------------------------------------------------------------------------

bool FakeRtlDevice::open(const std::string& device, std::string* error) {
  (void)device;
  if (opt_.fail_open) {
    if (error) *error = "fake device: open refused";
    return false;
  }
  if (!(opt_.speed > 0)) {
    if (error) *error = "fake device: speed must be > 0";
    return false;
  }
  in_ = std::fopen(opt_.path.c_str(), "rb");
  if (!in_) {
    if (error) *error = "fake device: cannot open " + opt_.path;
    return false;
  }
  return true;
}

void FakeRtlDevice::close() noexcept {
  if (in_) {
    std::fclose(in_);
    in_ = nullptr;
    ++closes_;
  }
}

int FakeRtlDevice::set_sample_rate(std::uint32_t hz) {
  if (!rtl_rate_ok(hz)) return -22;
  rate_ = hz;
  return 0;
}

std::vector<int> FakeRtlDevice::tuner_gains() const {
  // An R820T's steps.
  return {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
          280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
}

int FakeRtlDevice::set_tuner_gain(int tenth_db) {
  const std::vector<int> g = tuner_gains();
  if (std::find(g.begin(), g.end(), tenth_db) == g.end()) return -1;
  gain_ = tenth_db;
  return 0;
}

int FakeRtlDevice::read_async(Callback cb, void* ctx, std::uint32_t buf_num, std::uint32_t buf_len) {
  (void)buf_num;
  if (!in_ || buf_len == 0) return -1;
  buf_len_ = buf_len;
  std::vector<unsigned char> buf(buf_len);
  const double ns_per_byte = 1e9 / (static_cast<double>(rate_) * 2.0 * opt_.speed);
  const std::uint64_t t0 = mono_ns();
  auto cancelled = [&] { return cancel_.load(std::memory_order_acquire); };
  // Waits until `due` (or a cancel), in slices of at most 1 ms.
  auto wait_until = [&](std::uint64_t due) {
    for (;;) {
      if (cancelled()) return false;
      const std::uint64_t now = mono_ns();
      if (now >= due) return true;
      std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<std::uint64_t>(due - now, 1'000'000)));
    }
  };
  for (;;) {
    if (cancelled()) return 0;
    if (opt_.stall_after_bytes && delivered_ >= opt_.stall_after_bytes) {
      while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return 0;
    }
    if (opt_.error_after_bytes && delivered_ >= opt_.error_after_bytes) return -1;
    if (opt_.unplug_after_bytes && delivered_ >= opt_.unplug_after_bytes) return 0;
    std::size_t have = 0;
    while (have < buf_len) {
      const std::size_t got = std::fread(buf.data() + have, 1, buf_len - have, in_);
      if (got == 0) break;
      have += got;
    }
    if (have == 0) return kEndOfInput;
    // The transfer arrives when its last byte would have.
    const auto due = t0 + static_cast<std::uint64_t>(static_cast<double>(delivered_ + have) * ns_per_byte) +
                     (delivered_ == 0 ? opt_.hold_first_ns : 0u);
    if (!wait_until(due)) return 0;
    cb(buf.data(), static_cast<std::uint32_t>(have), ctx);
    delivered_ += have;
    if (have < buf_len) return kEndOfInput;
  }
}

#if !ADSB_HAVE_RTLSDR
std::unique_ptr<RtlDevice> make_librtlsdr_device() { return nullptr; }
bool rtlsdr_built() noexcept { return false; }
#endif

}  // namespace adsb::ingest
