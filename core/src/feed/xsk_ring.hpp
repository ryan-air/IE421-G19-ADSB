#pragma once

// Internal to the AF_XDP transports (xdp_sender.cpp, xdp_receiver.cpp): one
// ring shared with the kernel, mapped from an XSK socket. Linux only.

#include <linux/if_xdp.h>
#include <sys/mman.h>
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
