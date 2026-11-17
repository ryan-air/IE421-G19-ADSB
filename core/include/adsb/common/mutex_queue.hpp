#pragma once

// Bounded queue with a mutex and std::deque: the deliberately naive baseline
// for SpscRing (README_UPDATES.md §11.5). Same interface as SpscRing so the
// pipeline code is identical; selected with -DADSB_NAIVE=ON.
//
// What makes it slow, on purpose: a lock per operation (and the cache-line
// ping-pong of the mutex itself), and deque node allocation as it grows.

#include <cstddef>
#include <deque>
#include <mutex>
#include <span>

namespace adsb {

template <typename T, std::size_t Capacity>
class MutexQueue {
 public:
  using value_type = T;
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t capacity() noexcept { return Capacity; }

  [[nodiscard]] bool try_push(const T& v) {
    std::lock_guard<std::mutex> lock(mu_);
    if (q_.size() >= Capacity) return false;
    q_.push_back(v);
    return true;
  }

  [[nodiscard]] std::size_t try_push_n(std::span<const T> values) {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    while (n < values.size() && q_.size() < Capacity) q_.push_back(values[n++]);
    return n;
  }

  [[nodiscard]] bool try_pop(T& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (q_.empty()) return false;
    out = q_.front();
    q_.pop_front();
    return true;
  }

  [[nodiscard]] std::size_t try_pop_n(std::span<T> out) {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t n = 0;
    while (n < out.size() && !q_.empty()) {
      out[n++] = q_.front();
      q_.pop_front();
    }
    return n;
  }

  [[nodiscard]] std::size_t size_approx() const {
    std::lock_guard<std::mutex> lock(mu_);
    return q_.size();
  }
  [[nodiscard]] bool empty_approx() const { return size_approx() == 0; }

 private:
  mutable std::mutex mu_;
  std::deque<T> q_;
};

}  // namespace adsb
