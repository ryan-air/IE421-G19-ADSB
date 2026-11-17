#pragma once

// Bounded, lock-free, single-producer / single-consumer ring buffer.
//
// Design (see README_UPDATES.md §4):
//  * Capacity is a compile-time power of two, so index -> slot is a mask.
//  * Indices are 64-bit and increase forever (no wrap handling needed:
//    at 10^9 ops/s a 64-bit counter lasts ~584 years).
//  * The producer's index and the consumer's index live on separate cache
//    lines. Each side also keeps a private *cached* copy of the other side's
//    index, so in the common case push/pop touch only their own cache line and
//    the slot itself; the shared index is re-read only when the ring looks
//    full (producer) or empty (consumer).
//  * Ordering: the producer writes the slot, then publishes the new write index
//    with a release store; the consumer reads the write index with an acquire
//    load before reading the slot (and symmetrically for freeing slots).
//  * Elements are trivially copyable and copied by value. No allocation after
//    construction; construct the ring at startup (e.g. std::make_unique).
//    Slots are zeroed on construction, which also pre-faults the memory.
//
// Thread-safety contract: exactly one thread may call the producer functions
// (try_push, try_push_n) and exactly one thread may call the consumer functions
// (try_pop, try_pop_n, front, pop). The observers (size_approx, empty_approx)
// may be called from any thread and return a possibly stale value.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "adsb/common/cache.hpp"

namespace adsb {

template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2, "capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "elements must be trivially copyable");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "64-bit atomics must be lock-free on this platform");

 public:
  using value_type = T;
  static constexpr std::size_t kCapacity = Capacity;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  static constexpr std::size_t capacity() noexcept { return Capacity; }

  // ---- Producer side ------------------------------------------------------

  // Returns false (and leaves the ring unchanged) if the ring is full.
  [[nodiscard]] bool try_push(const T& value) noexcept {
    const std::uint64_t w = prod_.write.load(std::memory_order_relaxed);
    if (w - prod_.cached_read == Capacity) {
      prod_.cached_read = cons_.read.load(std::memory_order_acquire);
      if (w - prod_.cached_read == Capacity) return false;
    }
    slots_[w & kMask] = value;
    prod_.write.store(w + 1, std::memory_order_release);
    return true;
  }

  // Pushes as many elements from `values` as fit; returns how many were pushed.
  // A single release store publishes the whole batch.
  [[nodiscard]] std::size_t try_push_n(std::span<const T> values) noexcept {
    const std::uint64_t w = prod_.write.load(std::memory_order_relaxed);
    std::uint64_t free_slots = Capacity - (w - prod_.cached_read);
    if (free_slots < values.size()) {
      prod_.cached_read = cons_.read.load(std::memory_order_acquire);
      free_slots = Capacity - (w - prod_.cached_read);
    }
    const std::size_t n = values.size() < free_slots ? values.size()
                                                     : static_cast<std::size_t>(free_slots);
    for (std::size_t i = 0; i < n; ++i) slots_[(w + i) & kMask] = values[i];
    if (n != 0) prod_.write.store(w + n, std::memory_order_release);
    return n;
  }

  // ---- Consumer side ------------------------------------------------------

  // Returns false (and leaves `out` unchanged) if the ring is empty.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::uint64_t r = cons_.read.load(std::memory_order_relaxed);
    if (r == cons_.cached_write) {
      cons_.cached_write = prod_.write.load(std::memory_order_acquire);
      if (r == cons_.cached_write) return false;
    }
    out = slots_[r & kMask];
    cons_.read.store(r + 1, std::memory_order_release);
    return true;
  }

  // Pops up to out.size() elements; returns how many were popped.
  [[nodiscard]] std::size_t try_pop_n(std::span<T> out) noexcept {
    const std::uint64_t r = cons_.read.load(std::memory_order_relaxed);
    std::uint64_t avail = cons_.cached_write - r;
    if (avail < out.size()) {
      cons_.cached_write = prod_.write.load(std::memory_order_acquire);
      avail = cons_.cached_write - r;
    }
    const std::size_t n = out.size() < avail ? out.size() : static_cast<std::size_t>(avail);
    for (std::size_t i = 0; i < n; ++i) out[i] = slots_[(r + i) & kMask];
    if (n != 0) cons_.read.store(r + n, std::memory_order_release);
    return n;
  }

  // Zero-copy peek at the oldest element, or nullptr if empty. The pointer is
  // valid until the consumer calls pop()/try_pop*().
  [[nodiscard]] const T* front() noexcept {
    const std::uint64_t r = cons_.read.load(std::memory_order_relaxed);
    if (r == cons_.cached_write) {
      cons_.cached_write = prod_.write.load(std::memory_order_acquire);
      if (r == cons_.cached_write) return nullptr;
    }
    return &slots_[r & kMask];
  }

  // Releases the element returned by front(). Precondition: front() != nullptr.
  void pop() noexcept {
    const std::uint64_t r = cons_.read.load(std::memory_order_relaxed);
    assert(r != cons_.cached_write && "pop() without a successful front()");
    cons_.read.store(r + 1, std::memory_order_release);
  }

  // ---- Observers (any thread, approximate) --------------------------------

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::uint64_t r = cons_.read.load(std::memory_order_acquire);
    const std::uint64_t w = prod_.write.load(std::memory_order_acquire);
    // r may be loaded before a concurrent pop and w after a concurrent push, so
    // clamp to [0, Capacity].
    if (w <= r) return 0;
    const std::uint64_t n = w - r;
    return n > Capacity ? Capacity : static_cast<std::size_t>(n);
  }

  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  static constexpr std::uint64_t kMask = Capacity - 1;

  // Producer-owned line: the write index plus the producer's cached view of
  // the read index.
  struct alignas(kCacheLine) ProducerState {
    std::atomic<std::uint64_t> write{0};
    std::uint64_t cached_read{0};
  };
  // Consumer-owned line.
  struct alignas(kCacheLine) ConsumerState {
    std::atomic<std::uint64_t> read{0};
    std::uint64_t cached_write{0};
  };

  ProducerState prod_;
  ConsumerState cons_;
  // Value-initialized: a one-time cost at construction that also touches
  // every page, so the hot path never takes a first-touch page fault.
  alignas(kCacheLine) T slots_[Capacity]{};
};

}  // namespace adsb
