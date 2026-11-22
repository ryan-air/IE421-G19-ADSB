#pragma once

// Single-producer / single-consumer ring of IQ sample blocks (plan §5.1).
//
// The producer is the SDR (or file) reader: it fills a block in place and
// commits it; the consumer (the demodulator) reads it in place and releases
// it. When every slot is in use the producer does not wait: it drops the
// block and counts it. That is the SDR -> IQ ring overload boundary: an SDR
// cannot be paused, so when the demodulator falls behind, whole blocks of
// samples are lost, and the consumer sees the gap in the sample index
// (README_UPDATES.md §10.1, "drop and count").
//
// Memory: slots * block_bytes, allocated once. Ordering: the producer's
// commit is a release store of the head after the block and its header are
// written; the consumer's acquire load of the head makes them visible. The
// tail works the same way in the other direction, so a released slot can be
// refilled. Indices count up forever (64-bit), slot = index % slots.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include "adsb/common/cache.hpp"

namespace adsb::ingest {

struct IqBlock {
  std::uint8_t* data = nullptr;   // block_bytes of samples
  std::size_t samples = 0;        // complex samples in this block
  std::uint64_t first_sample = 0; // absolute index of data[0] in the stream
  std::uint64_t host_ns = 0;      // mono_ns() when the block arrived (its last sample)
};

class IqBlockRing {
 public:
  IqBlockRing(std::size_t slots, std::size_t block_bytes)
      : slots_(slots < 2 ? 2 : slots),
        block_bytes_(block_bytes),
        storage_(std::make_unique<std::uint8_t[]>(slots_ * block_bytes)),
        blocks_(std::make_unique<IqBlock[]>(slots_)) {
    for (std::size_t i = 0; i < slots_; ++i) blocks_[i].data = storage_.get() + i * block_bytes_;
  }
  IqBlockRing(const IqBlockRing&) = delete;
  IqBlockRing& operator=(const IqBlockRing&) = delete;

  [[nodiscard]] std::size_t slots() const noexcept { return slots_; }
  [[nodiscard]] std::size_t block_bytes() const noexcept { return block_bytes_; }

  // Producer: the next free slot, or nullptr if the ring is full.
  [[nodiscard]] IqBlock* acquire() noexcept {
    const std::uint64_t h = head_.v.load(std::memory_order_relaxed);
    if (h - tail_cache_ >= slots_) {
      tail_cache_ = tail_.v.load(std::memory_order_acquire);
      if (h - tail_cache_ >= slots_) return nullptr;
    }
    return &blocks_[h % slots_];
  }
  // Producer: publishes the block returned by acquire().
  void commit() noexcept {
    const std::uint64_t h = head_.v.load(std::memory_order_relaxed) + 1;
    head_.v.store(h, std::memory_order_release);
  }

  // Consumer: the oldest committed block, or nullptr if none.
  [[nodiscard]] const IqBlock* peek() noexcept {
    const std::uint64_t t = tail_.v.load(std::memory_order_relaxed);
    if (t == head_cache_) {
      head_cache_ = head_.v.load(std::memory_order_acquire);
      if (t == head_cache_) return nullptr;
    }
    return &blocks_[t % slots_];
  }
  // Consumer: frees the block returned by peek().
  void release() noexcept {
    const std::uint64_t t = tail_.v.load(std::memory_order_relaxed) + 1;
    tail_.v.store(t, std::memory_order_release);
  }

  // Blocks committed and not yet released (approximate from either side).
  [[nodiscard]] std::size_t depth_approx() const noexcept {
    const std::uint64_t h = head_.v.load(std::memory_order_acquire);
    const std::uint64_t t = tail_.v.load(std::memory_order_acquire);
    return h >= t ? static_cast<std::size_t>(h - t) : 0;
  }

 private:
  struct alignas(kCacheLine) Index {
    std::atomic<std::uint64_t> v{0};
  };
  const std::size_t slots_;
  const std::size_t block_bytes_;
  std::unique_ptr<std::uint8_t[]> storage_;
  std::unique_ptr<IqBlock[]> blocks_;
  Index head_;                       // written by the producer
  alignas(kCacheLine) std::uint64_t tail_cache_ = 0;  // producer's copy of tail
  Index tail_;                       // written by the consumer
  alignas(kCacheLine) std::uint64_t head_cache_ = 0;  // consumer's copy of head
};

}  // namespace adsb::ingest
