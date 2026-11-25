#include "adsb/ingest/iq_source.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace adsb::ingest {

namespace {

demod::DemodConfig demod_config(const IqSourceConfig& c) {
  demod::DemodConfig d = c.demod;
  d.sample_rate_hz = c.sample_rate_hz;
  d.block_samples = std::max<std::size_t>(c.block_samples, 1024);
  return d;
}

// The value of "key" in a JSON text (first occurrence; strings unquoted).
bool json_value(const std::string& text, const std::string& key, std::string& out) {
  const std::size_t k = text.find("\"" + key + "\"");
  if (k == std::string::npos) return false;
  std::size_t i = text.find(':', k + key.size() + 2);  // after the key (which may hold a colon)
  if (i == std::string::npos) return false;
  ++i;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\t' || text[i] == '\r')) ++i;
  if (i < text.size() && text[i] == '"') {
    const std::size_t e = text.find('"', i + 1);
    if (e == std::string::npos) return false;
    out = text.substr(i + 1, e - i - 1);
    return true;
  }
  std::size_t e = i;
  while (e < text.size() && text[e] != ',' && text[e] != '}' && text[e] != '\n') ++e;
  out = text.substr(i, e - i);
  return !out.empty();
}

// A device delivers cu8 in USB transfers of a multiple of 512 bytes.
bool device_config_ok(const IqSourceConfig& c, std::string* why) {
  auto fail = [&](const char* m) {
    if (why) *why = m;
    return false;
  };
  if (c.format != demod::SampleFormat::kCu8) return fail("an RTL-SDR delivers cu8 samples");
  if ((c.block_samples * 2) % 512 != 0) {
    return fail("an RTL-SDR's USB transfer must be a multiple of 512 bytes: block_samples must be a multiple of 256");
  }
  if (!rtl_rate_ok(c.sample_rate_hz)) return fail("an RTL-SDR's sample rate must be in 225-300 kHz or 0.9-3.2 MHz");
  if (c.rtl.stall_timeout_ns < 1'000'000) return fail("rtl.stall_timeout_ns must be at least 1 ms");
  return true;
}

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

bool validate(const IqSourceConfig& c, std::string* why) {
  auto fail = [&](const char* m) {
    if (why) *why = m;
    return false;
  };
  if (c.path.empty()) return fail("no input path");
  if (c.block_samples < 256 || c.block_samples > (std::size_t{1} << 22)) return fail("block_samples must be in [256, 4194304]");
  if (c.buffers < 2 || c.buffers > 4096) return fail("buffers must be in [2, 4096]");
  // Paced faster than this, event time is compressed so much that repeated
  // replies fall inside the engine's 2 ms duplicate window.
  if (!(c.speed >= 0.0 && c.speed <= 200.0)) return fail("speed must be in [0, 200] (0 = unpaced)");
  if (c.speed > 0 && c.speed < 1e-3) return fail("speed must be at least 0.001 (or 0 = unpaced)");
  if (c.timing_window < 1 || c.timing_window > 65536) return fail("timing_window must be in [1, 65536]");
  if (parse_rtlsdr_path(c.path) && !device_config_ok(c, why)) return false;
  return demod::validate(demod_config(c), why);
}

bool read_sigmf(const std::string& path, std::string& data_path, demod::SampleFormat& format, double& rate,
                std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  if (!ends_with(path, ".sigmf-meta") && !ends_with(path, ".sigmf-data")) return fail(path + ": not a SigMF file name");
  const std::string base = path.substr(0, path.size() - 11);
  std::ifstream mf(base + ".sigmf-meta");
  if (!mf) return fail("cannot read " + base + ".sigmf-meta");
  std::stringstream ss;
  ss << mf.rdbuf();
  const std::string meta = ss.str();
  std::string dt, r;
  if (!json_value(meta, "core:datatype", dt) || !demod::parse_format(dt, format)) {
    return fail("unsupported or missing core:datatype in " + base + ".sigmf-meta");
  }
  if (!json_value(meta, "core:sample_rate", r)) return fail("no core:sample_rate in " + base + ".sigmf-meta");
  char* end = nullptr;
  rate = std::strtod(r.c_str(), &end);
  if (end == r.c_str() || !(rate > 0)) return fail("bad core:sample_rate in " + base + ".sigmf-meta");
  data_path = base + ".sigmf-data";
  return true;
}

namespace {
// Nothing is sized from an invalid configuration (open() then reports it).
IqSourceConfig usable(const IqSourceConfig& c, bool valid) {
  if (valid) return c;
  IqSourceConfig d;
  d.path = c.path;
  return d;
}
}  // namespace

IqSource::IqSource(IqSourceConfig cfg)
    : cfg_(std::move(cfg)), valid_(validate(cfg_, &invalid_why_)), demod_(demod_config(usable(cfg_, valid_))) {
  const IqSourceConfig c = usable(cfg_, valid_);
  ring_ = std::make_unique<IqBlockRing>(c.buffers, c.block_samples * demod::bytes_per_sample(c.format));
  // Arrival-time stamps (a paced file, or a live receiver on a pipe that
  // drops when full): samples arrive at the rate times the pacing speed.
  // An unpaced file that waits has no arrival clock: its frames are timed
  // on the sample clock from the first block (event time stays faithful;
  // latency is then meaningless).
  arrival_clock_ = c.speed > 0 || c.drop_when_full;
  ns_per_sample_ = 1e9 / (c.sample_rate_hz * (c.speed > 0 ? c.speed : 1.0));
  offsets_.assign(std::max<std::size_t>(c.timing_window, 1), 0.0);
}

IqSource::~IqSource() {
  join_reader();
  finish_device();
  if (own_in_ && in_) std::fclose(in_);
}

bool IqSource::open(std::string* error) {
  if (!valid_) {
    if (error) *error = invalid_why_;
    return false;
  }
  if (device_ || parse_rtlsdr_path(cfg_.path)) return open_device(error);
  if (cfg_.path == "-") {
    in_ = stdin;
    own_in_ = false;
  } else {
    in_ = std::fopen(cfg_.path.c_str(), "rb");
    own_in_ = true;
    if (!in_) {
      if (error) *error = "cannot open " + cfg_.path;
      return false;
    }
  }
  return true;
}

void IqSource::start_reader(const std::atomic<bool>& stop) {
  reader_done_.store(false);
  if (device_open_) {
    stop_ = &stop;
    last_transfer_ns_.store(mono_ns(), std::memory_order_relaxed);
    reader_ = std::thread([this, &stop] { device_loop(stop); });
    return;
  }
  if (!in_) {  // open() was not called or failed: nothing to read
    read_error_.store(true);
    reader_done_.store(true, std::memory_order_release);
    return;
  }
  reader_ = std::thread([this, &stop] { reader_loop(stop); });
}

void IqSource::join_reader() {
  if (reader_.joinable()) reader_.join();
}

void IqSource::reader_loop(const std::atomic<bool>& stop) {
  const std::size_t bps = demod::bytes_per_sample(cfg_.format);
  const std::size_t block_bytes = cfg_.block_samples * bps;
  std::vector<std::uint8_t> scratch(block_bytes);  // a dropped block is still read (and discarded)
  const bool paced = cfg_.speed > 0;
  const double ns_per_sample_paced = paced ? 1e9 / (cfg_.sample_rate_hz * cfg_.speed) : 0.0;
  std::uint64_t index = 0;   // absolute index of the next sample
  std::uint64_t t0 = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    // Paced: the block "arrives" when its last sample would have.
    std::uint64_t due = 0;
    if (paced) {
      if (t0 == 0) t0 = mono_ns();
      due = t0 + static_cast<std::uint64_t>(static_cast<double>(index + cfg_.block_samples) * ns_per_sample_paced);
      for (;;) {
        const std::uint64_t now = mono_ns();
        if (now >= due || stop.load(std::memory_order_relaxed)) break;
        if (due - now > 400'000) {  // sleep in slices, so stop is seen within 10 ms
          std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<std::uint64_t>(due - now - 300'000, 10'000'000)));
        } else {
          cpu_relax();
        }
      }
    }
    IqBlock* slot = ring_->acquire();
    while (slot == nullptr && !paced && !cfg_.drop_when_full && !stop.load(std::memory_order_relaxed)) {
      cpu_relax();
      slot = ring_->acquire();
    }
    std::uint8_t* dst = slot ? slot->data : scratch.data();
    // fread keeps reading until the block is full or the input ends, so only
    // the last block can be short (a trailing partial sample is ignored).
    std::size_t have = 0;
    while (have < block_bytes) {
      const std::size_t got = std::fread(dst + have, 1, block_bytes - have, in_);
      if (got == 0) break;
      have += got;
    }
    if (std::ferror(in_)) read_error_.store(true);
    const std::size_t n = have / bps;
    if (n == 0) break;
    const std::uint64_t now = mono_ns();
    if (paced && now > due) max_late_ = std::max(max_late_, now - due);
    ++blocks_;
    samples_ += n;
    if (slot) {
      slot->samples = n;
      slot->first_sample = index;
      slot->host_ns = now;
      ring_->commit();
    } else {
      ++blocks_dropped_;
      samples_dropped_ += n;
    }
    index += n;
    if (have < block_bytes) break;  // end of input
  }
  reader_done_.store(true, std::memory_order_release);
}

void IqSource::note_arrival(const IqBlock& b) noexcept {
  if (!arrival_clock_) {
    if (n_offsets_++ == 0) offset_ns_ = static_cast<double>(b.host_ns) - static_cast<double>(b.first_sample + b.samples) * ns_per_sample_;
    return;
  }
  const double off = static_cast<double>(b.host_ns) - static_cast<double>(b.first_sample + b.samples) * ns_per_sample_;
  offsets_[n_offsets_ % offsets_.size()] = off;
  ++n_offsets_;
  const std::size_t n = std::min(n_offsets_, offsets_.size());
  offset_ns_ = *std::min_element(offsets_.begin(), offsets_.begin() + static_cast<std::ptrdiff_t>(n));
}

record::Record IqSource::to_record(const demod::DemodFrame& f) const noexcept {
  record::Record r;
  const double spu = cfg_.sample_rate_hz / 1e6;
  const double end_sample = f.sample_pos + (8.0 + 8.0 * f.len) * spu;
  r.rx_ns = host_ns_of(end_sample);
  r.radio_ts = static_cast<std::uint64_t>(std::llround(std::max(0.0, f.sample_pos) * 12e6 / cfg_.sample_rate_hz)) &
               0xFFFFFFFFFFFFull;
  r.line = cfg_.line;
  r.type = f.len == 7 ? '2' : '3';
  r.signal = static_cast<std::uint8_t>(std::clamp(std::lround(f.signal * 255.0f), 0L, 255L));
  r.len = f.len;
  std::memcpy(r.payload, f.bytes, f.len);
  return r;
}

IqSourceStats IqSource::stats() const noexcept {
  IqSourceStats s;
  s.blocks = blocks_;
  s.samples = samples_;
  s.blocks_dropped = blocks_dropped_;
  s.samples_dropped = samples_dropped_;
  s.ring_high_water = high_water_;
  s.max_lateness_ns = max_late_;
  s.frames = frames_;
  s.time_clamped = clamped_;
  s.read_error = read_error_.load();
  s.device = device_mode_;
  s.demod = demod_.stats();
  return s;
}

// ---------------------------------------------------------------------------
// Device input
// ---------------------------------------------------------------------------

bool IqSource::open_device(std::string* error) {
  if (!device_config_ok(cfg_, error)) return false;
  // The device from the path ("rtlsdr" = index 0); a device handed over
  // with use_device() under another name opens as index 0.
  std::string dev_name = "0";
  (void)parse_rtlsdr_path(cfg_.path, &dev_name);
  if (!device_) {
    device_ = make_librtlsdr_device();
    if (!device_) {
      if (error) {
        *error = "built without librtlsdr (install librtlsdr-dev and pkg-config, then configure with -DADSB_RTLSDR=ON)";
      }
      return false;
    }
  }
  if (!device_->open(dev_name, error)) return false;
  device_open_ = true;
  if (!configure_rtl(*device_, cfg_.rtl, cfg_.sample_rate_hz, &info_, error)) {
    finish_device();
    return false;
  }
  if (!(device_->pace() > 0)) {
    if (error) *error = "device pace must be > 0";
    finish_device();
    return false;
  }
  device_mode_ = true;
  // A live receiver: frames are timed by arrival, at the device's pace.
  arrival_clock_ = true;
  ns_per_sample_ = 1e9 / (cfg_.sample_rate_hz * device_->pace());
  return true;
}

void IqSource::device_loop(const std::atomic<bool>& stop) {
  int r = 0;
  if (!stop.load(std::memory_order_relaxed)) {
    r = device_->read_async(&IqSource::on_transfer, this, cfg_.rtl.usb_buffers,
                            static_cast<std::uint32_t>(ring_->block_bytes()));
  } else {
    cancel_requested_.store(true);
  }
  if (r == RtlDevice::kEndOfInput) {
    // The test device's file ended: a clean end, like a file.
  } else if (r < 0) {
    error_ = "reading from the device failed (librtlsdr error " + std::to_string(r) + ")";
    read_error_.store(true);
  } else if (!cancel_requested_.load()) {
    // librtlsdr returns 0 on its own when the device is lost.
    error_ = "the device stopped delivering samples (unplugged?)";
    read_error_.store(true);
  }
  reader_done_.store(true, std::memory_order_release);
}

void IqSource::on_transfer(unsigned char* buf, std::uint32_t len, void* ctx) {
  static_cast<IqSource*>(ctx)->transfer(buf, len);
}

// The USB callback: copy into free blocks, never wait.
void IqSource::transfer(const unsigned char* buf, std::size_t len) noexcept {
  const std::uint64_t now = mono_ns();
  last_transfer_ns_.store(now, std::memory_order_relaxed);
  if (stop_->load(std::memory_order_relaxed)) {
    request_cancel();
    return;
  }
  const std::size_t block_bytes = ring_->block_bytes();
  std::size_t off = 0;
  while (len - off >= 2) {
    const std::size_t chunk = std::min(len - off, block_bytes) & ~std::size_t{1};
    const std::size_t n = chunk / 2;
    ++blocks_;
    samples_ += n;
    IqBlock* slot = ring_->acquire();
    if (slot) {
      std::memcpy(slot->data, buf + off, chunk);
      slot->samples = n;
      slot->first_sample = index_;
      slot->host_ns = now;
      ring_->commit();
    } else {
      ++blocks_dropped_;
      samples_dropped_ += n;
    }
    index_ += n;
    off += chunk;
  }
}

void IqSource::request_cancel() noexcept {
  cancel_requested_.store(true);
  (void)device_->cancel_async();
}

// Consumer side, every loop: on stop or a stall, cancel (repeated every
// 10 ms: librtlsdr ignores a cancel that comes before streaming started).
void IqSource::poll_device(const std::atomic<bool>& stop, bool idle) noexcept {
  if (reader_done_.load(std::memory_order_relaxed)) return;
  const bool stopping = stop.load(std::memory_order_relaxed);
  if (!stopping && !stalled_ && !idle) return;
  const std::uint64_t now = mono_ns();
  if (!stopping && !stalled_) {
    const std::uint64_t last = last_transfer_ns_.load(std::memory_order_relaxed);
    if (now <= last || now - last <= cfg_.rtl.stall_timeout_ns) return;
    stalled_ = true;
    read_error_.store(true);
  }
  if (last_cancel_ns_ == 0 || now - last_cancel_ns_ >= 10'000'000) {
    last_cancel_ns_ = now;
    request_cancel();
  }
}

// After the reader has been joined: the device is closed on this thread, so
// no cancel can race with it.
void IqSource::finish_device() {
  if (stalled_ && error_.empty()) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "no samples from the device for %.1f s",
                  static_cast<double>(cfg_.rtl.stall_timeout_ns) * 1e-9);
    error_ = buf;
  }
  if (device_open_) {
    device_->close();
    device_open_ = false;
  }
}

}  // namespace adsb::ingest
