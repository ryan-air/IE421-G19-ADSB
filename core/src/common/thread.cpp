#include "adsb/common/thread.hpp"

#include <pthread.h>

#include <cstddef>

#if defined(__linux__)
#include <sched.h>
#endif

namespace adsb {

bool pin_current_thread(int cpu) noexcept {
#if defined(__linux__)
  if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

void set_current_thread_name(const char* name) noexcept {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), name);  // truncated to 15 chars by the kernel
#elif defined(__APPLE__)
  pthread_setname_np(name);
#else
  (void)name;
#endif
}

}  // namespace adsb
