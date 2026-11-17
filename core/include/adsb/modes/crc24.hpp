#pragma once

// Mode S CRC-24 (ICAO Annex 10 Vol IV, generator polynomial 0x1FFF409).
//
// The parity field of a Mode S frame is the remainder of (data * x^24) mod G,
// computed MSB-first with a zero initial value and no final XOR. For a frame
// of n bytes, the *syndrome* is
//
//     syndrome = crc24(frame[0 .. n-3)) XOR frame[n-3 .. n)
//
// which is 0 for an error-free DF17/DF18 (parity = PI with II/SI = 0), and
// equals the 24-bit aircraft address for Address/Parity formats (DF0/4/5/16/
// 20/21), where the transmitter overlays the address on the parity.
//
// The byte-wise table is 256 x 4 bytes = 1 KB and is built at compile time.

#include <array>
#include <cstddef>
#include <cstdint>

namespace adsb::modes {

inline constexpr std::uint32_t kCrc24Poly = 0xFFF409;  // x^24 term implicit

namespace detail {
constexpr std::array<std::uint32_t, 256> make_crc24_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i << 16;
    for (int b = 0; b < 8; ++b) {
      c = (c & 0x800000u) ? ((c << 1) ^ kCrc24Poly) : (c << 1);
    }
    t[i] = c & 0xFFFFFFu;
  }
  return t;
}
inline constexpr std::array<std::uint32_t, 256> kCrc24Table = make_crc24_table();
}  // namespace detail

// Table-driven CRC-24 over `n` bytes.
[[nodiscard]] inline std::uint32_t crc24(const std::uint8_t* data, std::size_t n) noexcept {
  std::uint32_t crc = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t idx = ((crc >> 16) ^ data[i]) & 0xFFu;
    crc = ((crc << 8) ^ detail::kCrc24Table[idx]) & 0xFFFFFFu;
  }
  return crc;
}

// Syndrome of a complete frame (7 or 14 bytes). Precondition: n >= 4.
[[nodiscard]] inline std::uint32_t crc24_syndrome(const std::uint8_t* frame,
                                                  std::size_t n) noexcept {
  const std::uint32_t parity = (std::uint32_t{frame[n - 3]} << 16) |
                               (std::uint32_t{frame[n - 2]} << 8) | frame[n - 1];
  return crc24(frame, n - 3) ^ parity;
}

// Bit-at-a-time polynomial long division. Slow; used as an independent
// reference to validate the table-driven implementation.
[[nodiscard]] std::uint32_t crc24_bitwise(const std::uint8_t* data, std::size_t n) noexcept;

}  // namespace adsb::modes
