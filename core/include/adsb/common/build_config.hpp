#pragma once

// Selects the optimized or naive-baseline implementations at build time.
//
//   default            SpscRing + FlatTrackStore (lock-free, no allocation)
//   -DADSB_NAIVE=ON    MutexQueue + std::unordered_map (README_UPDATES.md §11.5)
//
// Pipeline code uses only these aliases, so both builds run identical logic
// and the benchmark difference is purely the data-structure choice.

#include <cstddef>

#include "adsb/common/mutex_queue.hpp"
#include "adsb/common/spsc_ring.hpp"
#include "adsb/state/track_engine.hpp"

namespace adsb {

#if defined(ADSB_NAIVE) && ADSB_NAIVE
inline constexpr bool kNaiveBuild = true;
template <typename T, std::size_t N>
using StageQueue = MutexQueue<T, N>;
using DefaultTrackEngine = state::NaiveTrackEngine;
#else
inline constexpr bool kNaiveBuild = false;
template <typename T, std::size_t N>
using StageQueue = SpscRing<T, N>;
using DefaultTrackEngine = state::TrackEngine;
#endif

inline constexpr const char* build_variant() noexcept { return kNaiveBuild ? "naive" : "optimized"; }

}  // namespace adsb
