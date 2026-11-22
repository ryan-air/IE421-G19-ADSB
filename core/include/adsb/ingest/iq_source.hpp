#pragma once

// IQ input for the live pipeline (plan §5.1): samples -> demodulator ->
// records, the same records BeastTcpSource produces, so adsb-live runs on
// raw samples instead of readsb's output.
//
// Two threads:
//   reader    reads blocks of `block_samples` from a file or stdin (an
//             RTL-SDR's USB transfers are the same thing: 16-256 KB of
//             samples at a time), paced at `speed` times the sample rate so
//             a recording behaves like a receiver (speed 0 = as fast as the
//             demodulator takes them; at most 200). Each block is stamped with the time it
//             arrived and its absolute sample index, and committed to an
//             IqBlockRing. When the ring is full the block is dropped and
//             counted (an SDR cannot be paused); without pacing the reader
//             waits instead, unless drop_when_full is set.
//   consumer  the thread calling run(): takes blocks, tells the demodulator
//             about gaps (Demodulator::skip), demodulates, and hands every
//             frame to the sink as a record::Record.
//
// Times. With arrival times (paced input, or unpaced with drop_when_full: a
// live receiver on a pipe), a frame's rx_ns is the host time its last
// sample arrived:
// host(i) = offset + i / (fs * speed), with the slope from the nominal rate and the
// offset the smallest (arrival - i / fs) of the last `timing_window` blocks.
// Delivery jitter only ever delays a block, so the lower envelope tracks the
// receiver's clock (plan §5.2: "a linear fit of sample index against host
// time, which absorbs USB callback jitter"); a sample-clock error of e ppm
// drifts it by e us/s within the window (40 ppm and 64 blocks of 6.8 ms:
// under 20 us). The latency from rx_ns to track state therefore includes
// the wait for the rest of the frame's block, which is what block size
// trades against overhead. An unpaced file read as fast as possible has no
// arrival clock: rx_ns is then the first block's arrival plus sample / fs,
// so event time (track expiry, duplicate windows) stays that of the
// recording, and the latency report means nothing. rx_ns never decreases from one frame to the next
// (it is raised to the previous one's when needed, and counted). radio_ts is the sample position on a 12 MHz
// clock, like readsb's Beast timestamps for file input.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "adsb/common/cache.hpp"
#include "adsb/common/clock.hpp"
#include "adsb/demod/demod.hpp"
#include "adsb/ingest/iq_ring.hpp"
#include "adsb/record/recording.hpp"

namespace adsb::ingest {

struct IqSourceConfig {
  std::string path;                       // raw sample file, or "-" for stdin
  demod::SampleFormat format = demod::SampleFormat::kCu8;
  double sample_rate_hz = 2.4e6;
  demod::DemodConfig demod;               // sample_rate_hz and block_samples are set from here
  std::size_t block_samples = 16384;      // per block (16384 cu8 samples = 32 KB, 6.8 ms at 2.4 MS/s)
  std::size_t buffers = 16;               // ring slots
  double speed = 1.0;                     // pacing: 1 = real time, 0 = unpaced (at most 200)
  // Unpaced only: drop when the ring is full instead of waiting (a live
  // receiver on a pipe, e.g. rtl_sdr -). Paced input always drops.
  bool drop_when_full = false;
  std::uint8_t line = 0;                  // receiver line id stamped on records
  std::size_t timing_window = 64;         // blocks in the arrival-time envelope
};

[[nodiscard]] bool validate(const IqSourceConfig& cfg, std::string* why = nullptr);

// Reads NAME.sigmf-meta (given NAME.sigmf-meta or NAME.sigmf-data): the data
// file, its format and sample rate.
[[nodiscard]] bool read_sigmf(const std::string& path, std::string& data_path, demod::SampleFormat& format,
                              double& sample_rate_hz, std::string* error);

struct IqSourceStats {
  std::uint64_t blocks = 0;               // read
  std::uint64_t samples = 0;              // read
  std::uint64_t blocks_dropped = 0;       // ring full
  std::uint64_t samples_dropped = 0;
  std::uint64_t ring_high_water = 0;      // blocks waiting, at most
  std::uint64_t max_lateness_ns = 0;      // paced: a block later than its due time, at most
  std::uint64_t frames = 0;               // handed to the sink
  std::uint64_t time_clamped = 0;         // frames whose rx_ns was raised to keep time monotonic
  bool read_error = false;
  demod::DemodStats demod{};
};

class IqSource {
 public:
  explicit IqSource(IqSourceConfig cfg);
  ~IqSource();
  IqSource(const IqSource&) = delete;
  IqSource& operator=(const IqSource&) = delete;

  [[nodiscard]] bool open(std::string* error);

  // Runs until the input ends or `stop` is set; calls sink(const Record&)
  // for every frame and on_status(const char*) when reading starts and ends.
  // A reader blocked in a read (stdin with nothing to read) sees `stop` only
  // when that read returns.
  template <typename Sink, typename Status>
  void run(const std::atomic<bool>& stop, Sink&& sink, Status&& on_status) {
    start_reader(stop);
    on_status("reading samples");
    std::uint64_t idle = 0;
    for (;;) {
      const IqBlock* b = ring_->peek();
      if (b == nullptr) {
        if (reader_done_.load(std::memory_order_acquire)) {
          b = ring_->peek();
          if (b == nullptr) break;
        } else {
          // A paced source is idle most of the time: spin briefly, then nap.
          if (++idle < 2048) {
            cpu_relax();
          } else {
            std::this_thread::sleep_for(std::chrono::microseconds(20));
          }
          continue;
        }
      }
      idle = 0;
      const std::size_t depth = ring_->depth_approx();
      if (depth > high_water_) high_water_ = depth;
      if (b->first_sample > demod_.position()) emit(demod_.skip(b->first_sample - demod_.position()), sink);
      note_arrival(*b);
      emit(demod_.feed(b->data, b->samples, cfg_.format), sink);
      ring_->release();
    }
    join_reader();
    emit(demod_.flush(), sink);
    on_status(read_error_.load() ? "read error" : "end of input");
  }

  template <typename Sink>
  void run(const std::atomic<bool>& stop, Sink&& sink) {
    run(stop, std::forward<Sink>(sink), [](const char*) {});
  }

  // Read after run() returns.
  [[nodiscard]] IqSourceStats stats() const noexcept;
  [[nodiscard]] const IqSourceConfig& config() const noexcept { return cfg_; }
  // Host time (mono_ns) of absolute sample index i, from the envelope.
  [[nodiscard]] std::uint64_t host_ns_of(double sample) const noexcept {
    const double t = offset_ns_ + sample * ns_per_sample_;
    return t > 0 ? static_cast<std::uint64_t>(t) : 0;
  }

 private:
  void start_reader(const std::atomic<bool>& stop);
  void join_reader();
  void reader_loop(const std::atomic<bool>& stop);
  void note_arrival(const IqBlock& b) noexcept;
  record::Record to_record(const demod::DemodFrame& f) const noexcept;

  template <typename Sink>
  void emit(std::span<const demod::DemodFrame> frames, Sink& sink) {
    for (const demod::DemodFrame& f : frames) {
      record::Record r = to_record(f);
      // Event time never goes backwards (the envelope can step down, and a
      // short reply can end before an overlapping longer one that started
      // first): the state engine and recordings rely on that.
      if (r.rx_ns < last_rx_ns_) {
        r.rx_ns = last_rx_ns_;
        ++clamped_;
      }
      last_rx_ns_ = r.rx_ns;
      ++frames_;
      sink(static_cast<const record::Record&>(r));
    }
  }

  IqSourceConfig cfg_;
  std::string invalid_why_;
  bool valid_ = false;
  bool arrival_clock_ = true;             // rx_ns from arrivals (else from the sample clock)
  FILE* in_ = nullptr;
  bool own_in_ = false;
  std::unique_ptr<IqBlockRing> ring_;
  demod::Demodulator demod_;
  std::thread reader_;
  std::atomic<bool> reader_done_{false};
  std::atomic<bool> read_error_{false};
  // Reader-side counters (read after join).
  std::uint64_t blocks_ = 0, samples_ = 0, blocks_dropped_ = 0, samples_dropped_ = 0, max_late_ = 0;
  // Consumer side.
  std::uint64_t frames_ = 0, high_water_ = 0, last_rx_ns_ = 0, clamped_ = 0;
  double ns_per_sample_ = 0.0;
  double offset_ns_ = 0.0;
  std::vector<double> offsets_;           // ring of (arrival - end_sample * ns_per_sample)
  std::size_t n_offsets_ = 0;
};

}  // namespace adsb::ingest
