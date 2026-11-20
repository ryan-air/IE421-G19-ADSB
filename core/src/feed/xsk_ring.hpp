#pragma once

// Internal to the AF_XDP transports (xdp_sender.cpp, xdp_receiver.cpp): one
// ring shared with the kernel, mapped from an XSK socket. Linux only.

#include <linux/if_xdp.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef AF_XDP
#define AF_XDP 44
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif

namespace adsb::feed::xsk {

inline void set_error(std::string* error, const std::string& what, int err) {
  if (error) *error = what + ": " + std::strerror(err);
}

// XDP_UMEM_REG failed. ENOBUFS / EPERM / ENOMEM there usually mean the
// pinned-memory limit: the kernel charges every UMEM to the user's
// RLIMIT_MEMLOCK (shared by all of that user's processes, and released a
// little after a socket closes) unless the process has CAP_IPC_LOCK.
inline void set_umem_error(std::string* error, std::size_t umem_bytes, int err) {
  if (!error) return;
  std::string what = "XDP_UMEM_REG: " + std::string(std::strerror(err));
  rlimit lim{};
  if ((err == ENOBUFS || err == EPERM || err == ENOMEM) && ::getrlimit(RLIMIT_MEMLOCK, &lim) == 0) {
    what += " (this UMEM pins " + std::to_string(umem_bytes >> 10) + " KiB; RLIMIT_MEMLOCK is ";
    what += lim.rlim_cur == RLIM_INFINITY ? std::string("unlimited") : std::to_string(lim.rlim_cur >> 10) + " KiB";
    what += " for all of this user's pinned memory: use fewer frames, raise ulimit -l, or grant CAP_IPC_LOCK)";
  }
  *error = what;
}

// We own one index (producer or consumer), the kernel the other.
struct Ring {
  std::uint32_t* producer = nullptr;
  std::uint32_t* consumer = nullptr;
  std::uint32_t* flags = nullptr;
  void* desc = nullptr;
  std::uint32_t mask = 0;
  void* map = MAP_FAILED;
  std::size_t map_len = 0;

  Ring() = default;
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  bool mmap_ring(int fd, const xdp_ring_offset& off, std::uint32_t size, std::size_t entry, off_t pgoff) {
    map_len = off.desc + size * entry;
    map = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, pgoff);
    if (map == MAP_FAILED) return false;
    auto* base = static_cast<std::uint8_t*>(map);
    producer = reinterpret_cast<std::uint32_t*>(base + off.producer);
    consumer = reinterpret_cast<std::uint32_t*>(base + off.consumer);
    flags = reinterpret_cast<std::uint32_t*>(base + off.flags);
    desc = base + off.desc;
    mask = size - 1;
    return true;
  }
  ~Ring() {
    if (map != MAP_FAILED) ::munmap(map, map_len);
  }
};

inline std::uint32_t load_acquire(const std::uint32_t* p) noexcept { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void store_release(std::uint32_t* p, std::uint32_t v) noexcept { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

}  // namespace adsb::feed::xsk
