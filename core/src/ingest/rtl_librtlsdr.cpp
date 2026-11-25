// RtlDevice on librtlsdr. Compiled only when CMake finds librtlsdr
// (ADSB_HAVE_RTLSDR); see rtl_device.hpp.

#include <rtl-sdr.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "adsb/ingest/rtl_device.hpp"

namespace adsb::ingest {

namespace {

const char* tuner_name(enum rtlsdr_tuner t) {
  switch (t) {
    case RTLSDR_TUNER_E4000: return "E4000";
    case RTLSDR_TUNER_FC0012: return "FC0012";
    case RTLSDR_TUNER_FC0013: return "FC0013";
    case RTLSDR_TUNER_FC2580: return "FC2580";
    case RTLSDR_TUNER_R820T: return "R820T";
    case RTLSDR_TUNER_R828D: return "R828D";
    default: return "unknown";
  }
}

class LibRtlSdrDevice final : public RtlDevice {
 public:
  ~LibRtlSdrDevice() override { close(); }

  bool open(const std::string& device, std::string* error) override {
    close();
    const std::uint32_t count = rtlsdr_get_device_count();
    if (count == 0) {
      if (error) *error = "no RTL-SDR device found";
      return false;
    }
    // An index if it is all digits and in range, else a serial number.
    int index = -1;
    char* end = nullptr;
    const long n = std::strtol(device.c_str(), &end, 10);
    if (!device.empty() && *end == '\0' && n >= 0 && static_cast<std::uint32_t>(n) < count) {
      index = static_cast<int>(n);
    } else {
      index = rtlsdr_get_index_by_serial(device.c_str());
    }
    if (index < 0) {
      if (error) {
        *error = "no RTL-SDR with index or serial '" + device + "' (" + std::to_string(count) + " found:";
        for (std::uint32_t i = 0; i < count; ++i) {
          char m[256] = {}, p[256] = {}, s[256] = {};
          (void)rtlsdr_get_device_usb_strings(i, m, p, s);
          *error += " " + std::to_string(i) + "=" + s;
        }
        *error += ")";
      }
      return false;
    }
    const int r = rtlsdr_open(&dev_, static_cast<std::uint32_t>(index));
    if (r < 0 || dev_ == nullptr) {
      dev_ = nullptr;
      if (error) *error = "cannot open RTL-SDR " + std::to_string(index) + " (error " + std::to_string(r) +
                          "; in use by another program, or no USB permission?)";
      return false;
    }
    const char* nm = rtlsdr_get_device_name(static_cast<std::uint32_t>(index));
    name_ = nm ? nm : "RTL-SDR";
    char m[256] = {}, p[256] = {}, s[256] = {};
    if (rtlsdr_get_usb_strings(dev_, m, p, s) == 0) serial_ = s;
    return true;
  }

  void close() noexcept override {
    if (dev_) {
      (void)rtlsdr_close(dev_);
      dev_ = nullptr;
    }
  }

  [[nodiscard]] std::string name() const override { return name_; }
  [[nodiscard]] std::string serial() const override { return serial_; }
  [[nodiscard]] std::string tuner() const override { return dev_ ? tuner_name(rtlsdr_get_tuner_type(dev_)) : "none"; }
  int set_sample_rate(std::uint32_t hz) override { return dev_ ? rtlsdr_set_sample_rate(dev_, hz) : -1; }
  [[nodiscard]] std::uint32_t sample_rate() const override { return dev_ ? rtlsdr_get_sample_rate(dev_) : 0; }
  int set_center_freq(std::uint32_t hz) override { return dev_ ? rtlsdr_set_center_freq(dev_, hz) : -1; }
  [[nodiscard]] std::uint32_t center_freq() const override { return dev_ ? rtlsdr_get_center_freq(dev_) : 0; }
  int set_freq_correction(int ppm) override { return dev_ ? rtlsdr_set_freq_correction(dev_, ppm) : -1; }
  [[nodiscard]] std::vector<int> tuner_gains() const override {
    if (!dev_) return {};
    const int n = rtlsdr_get_tuner_gains(dev_, nullptr);
    if (n <= 0) return {};
    std::vector<int> g(static_cast<std::size_t>(n));
    const int m = rtlsdr_get_tuner_gains(dev_, g.data());
    g.resize(m > 0 ? static_cast<std::size_t>(m) : 0u);
    return g;
  }
  int set_tuner_gain_mode(bool manual) override { return dev_ ? rtlsdr_set_tuner_gain_mode(dev_, manual ? 1 : 0) : -1; }
  int set_tuner_gain(int tenth_db) override { return dev_ ? rtlsdr_set_tuner_gain(dev_, tenth_db) : -1; }
  [[nodiscard]] int tuner_gain() const override { return dev_ ? rtlsdr_get_tuner_gain(dev_) : 0; }
  int set_agc_mode(bool on) override { return dev_ ? rtlsdr_set_agc_mode(dev_, on ? 1 : 0) : -1; }
  int set_bias_tee(bool on) override { return dev_ ? rtlsdr_set_bias_tee(dev_, on ? 1 : 0) : -1; }
  int reset_buffer() override { return dev_ ? rtlsdr_reset_buffer(dev_) : -1; }
  int read_async(Callback cb, void* ctx, std::uint32_t buf_num, std::uint32_t buf_len) override {
    return dev_ ? rtlsdr_read_async(dev_, cb, ctx, buf_num, buf_len) : -1;
  }
  int cancel_async() override { return dev_ ? rtlsdr_cancel_async(dev_) : -1; }

 private:
  rtlsdr_dev_t* dev_ = nullptr;
  std::string name_, serial_;
};

}  // namespace

std::unique_ptr<RtlDevice> make_librtlsdr_device() { return std::make_unique<LibRtlSdrDevice>(); }
bool rtlsdr_built() noexcept { return true; }

}  // namespace adsb::ingest
