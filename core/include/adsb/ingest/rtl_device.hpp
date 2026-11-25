#pragma once

// RTL-SDR as a live IQ source (plan §5.1): the device behind a small
// interface, so IqSource streams from a real dongle (librtlsdr) or from a
// test device that plays a file the same way.
//
// The interface is librtlsdr's own calls, reduced to what the receiver
// needs: open, set_*, read_async (blocks, calling back once per USB
// transfer on the calling thread, until cancel_async or an error) and
// close. Return values follow librtlsdr: 0 = ok, negative = error.
//
// Built with librtlsdr only when CMake finds it (ADSB_RTLSDR=AUTO|ON|OFF,
// pkg-config librtlsdr; ADSB_HAVE_RTLSDR). Without it make_librtlsdr_device()
// returns nullptr and `--iq rtlsdr` says so.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace adsb::ingest {

// What to ask of the receiver.
enum class RtlGain : std::uint8_t {
  kMax,     // manual gain, the tuner's highest step (readsb's and dump1090-fa's default)
  kAuto,    // the tuner's own AGC (librtlsdr gain mode 0)
  kManual,  // manual gain, the supported step nearest gain_db
};

// The device itself is named by the IQ path: "rtlsdr" (index 0),
// "rtlsdr:INDEX" or "rtlsdr:SERIAL" (the serial number in its USB strings).
struct RtlConfig {
  double freq_hz = 1090e6;
  int ppm = 0;                       // crystal correction
  RtlGain gain = RtlGain::kMax;
  double gain_db = 0;                // kManual
  bool agc = false;                  // the RTL2832's digital AGC (readsb --enable-agc)
  bool bias_tee = false;             // power an LNA over the antenna cable
  std::uint32_t usb_buffers = 0;     // librtlsdr transfer buffers (0 = its default, 15)
  // No transfer for this long counts as a dead device (the run ends with an
  // error instead of waiting forever).
  std::uint64_t stall_timeout_ns = 3'000'000'000;
};

// What the device actually runs at (read back after configuring).
struct RtlInfo {
  std::string name;                  // e.g. "Generic RTL2832U OEM"
  std::string serial;
  std::string tuner;                 // e.g. "R820T"
  double sample_rate_hz = 0;
  double freq_hz = 0;
  double gain_db = 0;                // manual gain (0 with tuner AGC)
  bool gain_auto = false;
  int ppm = 0;
  bool agc = false;
  bool bias_tee = false;
};

class RtlDevice {
 public:
  using Callback = void (*)(unsigned char* buf, std::uint32_t len, void* ctx);
  // Returned by read_async when the input really ended (the test device at
  // the end of its file). librtlsdr never returns it: a real device ends
  // only when cancelled or lost.
  static constexpr int kEndOfInput = 1;

  virtual ~RtlDevice() = default;
  // `device`: an index, or a serial number.
  virtual bool open(const std::string& device, std::string* error) = 0;
  virtual void close() noexcept = 0;
  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual std::string serial() const = 0;
  [[nodiscard]] virtual std::string tuner() const = 0;
  virtual int set_sample_rate(std::uint32_t hz) = 0;
  [[nodiscard]] virtual std::uint32_t sample_rate() const = 0;
  virtual int set_center_freq(std::uint32_t hz) = 0;
  [[nodiscard]] virtual std::uint32_t center_freq() const = 0;
  virtual int set_freq_correction(int ppm) = 0;
  [[nodiscard]] virtual std::vector<int> tuner_gains() const = 0;  // tenths of a dB, ascending
  virtual int set_tuner_gain_mode(bool manual) = 0;
  virtual int set_tuner_gain(int tenth_db) = 0;
  [[nodiscard]] virtual int tuner_gain() const = 0;              // tenths of a dB
  virtual int set_agc_mode(bool on) = 0;
  virtual int set_bias_tee(bool on) = 0;
  virtual int reset_buffer() = 0;
  // Blocks; cb runs on this thread for every transfer of buf_len bytes.
  virtual int read_async(Callback cb, void* ctx, std::uint32_t buf_num, std::uint32_t buf_len) = 0;
  // Any thread, also from inside the callback; harmless when not streaming.
  virtual int cancel_async() = 0;
  // Pacing of the samples relative to the sample rate: 1 for a real device.
  [[nodiscard]] virtual double pace() const { return 1.0; }
};

// librtlsdr, or nullptr when built without it.
[[nodiscard]] std::unique_ptr<RtlDevice> make_librtlsdr_device();
[[nodiscard]] bool rtlsdr_built() noexcept;

// "rtlsdr", "rtlsdr:INDEX", "rtlsdr:SERIAL": true and the device part ("0"
// for plain "rtlsdr"); false for anything else.
[[nodiscard]] bool parse_rtlsdr_path(const std::string& path, std::string* device = nullptr);
// "max", "auto" or a gain in dB.
[[nodiscard]] bool parse_rtl_gain(const std::string& s, RtlGain& mode, double& db);
// The supported gain (tenths of a dB) nearest db; ties go to the lower step.
[[nodiscard]] int nearest_gain(const std::vector<int>& gains, double db) noexcept;
// librtlsdr's sample-rate ranges: 225,001-300,000 and 900,001-3,200,000 Hz.
[[nodiscard]] bool rtl_rate_ok(double hz) noexcept;

// Applies cfg and the sample rate to an opened device and reads back what it
// runs at.
[[nodiscard]] bool configure_rtl(RtlDevice& dev, const RtlConfig& cfg, double sample_rate_hz, RtlInfo* info,
                                 std::string* error);
[[nodiscard]] std::string rtl_info_text(const RtlInfo& info);

// ---------------------------------------------------------------------------
// Test device: plays a cu8 file in transfers, paced at `speed` times the
// sample rate it is set to, from the thread calling read_async (like
// librtlsdr). It can fail in the ways a real dongle does. adsb-live exposes
// it as `--iq fake:FILE` for scripts (TESTING ONLY).
// ---------------------------------------------------------------------------
struct FakeRtlOptions {
  std::string path;                  // cu8 samples
  double speed = 1.0;                // > 0
  std::uint64_t error_after_bytes = 0;   // read_async returns -1 (a USB error) after this many bytes
  std::uint64_t unplug_after_bytes = 0;  // returns 0 without being cancelled (librtlsdr on a lost device)
  std::uint64_t stall_after_bytes = 0;   // stops delivering; returns only when cancelled
  std::uint64_t hold_first_ns = 0;       // the first transfer comes this late (USB start-up), the rest on time
  bool fail_open = false;
};

class FakeRtlDevice final : public RtlDevice {
 public:
  explicit FakeRtlDevice(FakeRtlOptions opt) : opt_(std::move(opt)) {}
  ~FakeRtlDevice() override { close(); }

  bool open(const std::string& device, std::string* error) override;
  void close() noexcept override;
  [[nodiscard]] std::string name() const override { return "fake (" + opt_.path + ")"; }
  [[nodiscard]] std::string serial() const override { return "FAKE0001"; }
  [[nodiscard]] std::string tuner() const override { return "R820T"; }
  int set_sample_rate(std::uint32_t hz) override;
  [[nodiscard]] std::uint32_t sample_rate() const override { return rate_; }
  int set_center_freq(std::uint32_t hz) override {
    freq_ = hz;
    return 0;
  }
  [[nodiscard]] std::uint32_t center_freq() const override { return freq_; }
  int set_freq_correction(int ppm) override {
    if (ppm == ppm_) return -2;  // librtlsdr: unchanged
    ppm_ = ppm;
    return 0;
  }
  [[nodiscard]] std::vector<int> tuner_gains() const override;
  int set_tuner_gain_mode(bool manual) override {
    manual_ = manual;
    return 0;
  }
  int set_tuner_gain(int tenth_db) override;
  [[nodiscard]] int tuner_gain() const override { return manual_ ? gain_ : 0; }
  int set_agc_mode(bool on) override {
    agc_ = on;
    return 0;
  }
  int set_bias_tee(bool on) override {
    bias_ = on;
    return 0;
  }
  int reset_buffer() override {
    ++resets_;
    return 0;
  }
  int read_async(Callback cb, void* ctx, std::uint32_t buf_num, std::uint32_t buf_len) override;
  int cancel_async() override {
    cancel_calls_.fetch_add(1, std::memory_order_relaxed);
    cancel_.store(true, std::memory_order_release);
    return 0;
  }
  [[nodiscard]] double pace() const override { return opt_.speed; }

  // For tests (read after the run).
  [[nodiscard]] bool manual() const noexcept { return manual_; }
  [[nodiscard]] int ppm() const noexcept { return ppm_; }
  [[nodiscard]] bool agc() const noexcept { return agc_; }
  [[nodiscard]] bool bias_tee() const noexcept { return bias_; }
  [[nodiscard]] int resets() const noexcept { return resets_; }
  [[nodiscard]] bool is_open() const noexcept { return in_ != nullptr; }
  [[nodiscard]] int closes() const noexcept { return closes_; }
  [[nodiscard]] std::uint32_t last_buf_len() const noexcept { return buf_len_; }
  [[nodiscard]] std::uint64_t cancel_calls() const noexcept { return cancel_calls_.load(); }
  [[nodiscard]] std::uint64_t bytes_delivered() const noexcept { return delivered_; }

 private:
  FakeRtlOptions opt_;
  FILE* in_ = nullptr;
  std::uint32_t rate_ = 2'048'000, freq_ = 0, buf_len_ = 0;
  int ppm_ = 0, gain_ = 0, resets_ = 0, closes_ = 0;
  bool manual_ = false, agc_ = false, bias_ = false;
  std::uint64_t delivered_ = 0;
  std::atomic<bool> cancel_{false};
  std::atomic<std::uint64_t> cancel_calls_{0};
};

}  // namespace adsb::ingest
