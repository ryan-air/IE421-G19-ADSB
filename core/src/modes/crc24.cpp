#include "adsb/modes/crc24.hpp"

namespace adsb::modes {

std::uint32_t crc24_bitwise(const std::uint8_t* data, std::size_t n) noexcept {
  // Long division of (data * x^24) by the 25-bit generator 0x1FFF409,
  // one message bit at a time, keeping a 25-bit working remainder.
  constexpr std::uint32_t kGenerator = 0x1FFF409;
  std::uint32_t rem = 0;
  for (std::size_t i = 0; i < n; ++i) {
    for (int b = 7; b >= 0; --b) {
      rem = (rem << 1) | ((data[i] >> b) & 1u);
      if (rem & 0x1000000u) rem ^= kGenerator;
    }
  }
  // Append 24 zero bits (the x^24 multiplication).
  for (int b = 0; b < 24; ++b) {
    rem <<= 1;
    if (rem & 0x1000000u) rem ^= kGenerator;
  }
  return rem & 0xFFFFFFu;
}

}  // namespace adsb::modes
