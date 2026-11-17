#include "adsb/modes/altitude.hpp"

namespace adsb::modes {

std::uint16_t id13_to_mode_a(std::uint16_t id13) noexcept {
  // Bit positions in the 13-bit field (bit 12 = first transmitted).
  std::uint16_t a = 0;
  if (id13 & 0x1000) a |= 0x0010;  // C1
  if (id13 & 0x0800) a |= 0x1000;  // A1
  if (id13 & 0x0400) a |= 0x0020;  // C2
  if (id13 & 0x0200) a |= 0x2000;  // A2
  if (id13 & 0x0100) a |= 0x0040;  // C4
  if (id13 & 0x0080) a |= 0x4000;  // A4
  // 0x0040 is X (ID field) or M (AC field): not part of the code.
  if (id13 & 0x0020) a |= 0x0100;  // B1
  if (id13 & 0x0010) a |= 0x0001;  // D1 (ID field) / Q (AC field)
  if (id13 & 0x0008) a |= 0x0200;  // B2
  if (id13 & 0x0004) a |= 0x0002;  // D2
  if (id13 & 0x0002) a |= 0x0400;  // B4
  if (id13 & 0x0001) a |= 0x0004;  // D4
  return a;
}

std::optional<std::int32_t> gillham_to_feet(std::uint16_t mode_a) noexcept {
  // Bits outside the 12 code bits, or D1 (never used for altitude), make the
  // code invalid; so does an all-zero C group (C1 C2 C4 cannot all be 0).
  if ((mode_a & 0x8889u) != 0 || (mode_a & 0x0070u) == 0) return std::nullopt;

  // 100 ft part: C1 C2 C4 form a 5-state reflected code, values 1..5.
  unsigned hundreds = 0;
  if (mode_a & 0x0010) hundreds ^= 0x7;  // C1
  if (mode_a & 0x0020) hundreds ^= 0x3;  // C2
  if (mode_a & 0x0040) hundreds ^= 0x1;  // C4
  if ((hundreds & 5u) == 5u) hundreds ^= 2u;  // map 7 -> 5 (and 5 -> 7)
  if (hundreds > 5) return std::nullopt;

  // 500 ft part: D2 D4 A1 A2 A4 B1 B2 B4 are a standard Gray code.
  unsigned five_hundreds = 0;
  if (mode_a & 0x0002) five_hundreds ^= 0x0FF;  // D2
  if (mode_a & 0x0004) five_hundreds ^= 0x07F;  // D4
  if (mode_a & 0x1000) five_hundreds ^= 0x03F;  // A1
  if (mode_a & 0x2000) five_hundreds ^= 0x01F;  // A2
  if (mode_a & 0x4000) five_hundreds ^= 0x00F;  // A4
  if (mode_a & 0x0100) five_hundreds ^= 0x007;  // B1
  if (mode_a & 0x0200) five_hundreds ^= 0x003;  // B2
  if (mode_a & 0x0400) five_hundreds ^= 0x001;  // B4

  // The 100 ft sub-code runs in reverse on odd 500 ft steps.
  if (five_hundreds & 1u) hundreds = 6u - hundreds;

  // Units of 100 ft, offset so that the lowest code is -1200 ft.
  const auto units = static_cast<std::int32_t>(five_hundreds * 5u + hundreds) - 13;
  return units * 100;
}

std::optional<std::int32_t> decode_ac13(std::uint16_t ac13) noexcept {
  ac13 &= 0x1FFF;
  if (ac13 == 0) return std::nullopt;   // altitude not available
  if (ac13 & 0x0040) return std::nullopt;  // M bit: metric, unsupported
  if (ac13 & 0x0010) {
    // Q bit: remove M (bit 6) and Q (bit 4), leaving an 11-bit integer.
    const unsigned n = ((ac13 & 0x1F80u) >> 2) | ((ac13 & 0x0020u) >> 1) | (ac13 & 0x000Fu);
    return static_cast<std::int32_t>(n) * 25 - 1000;
  }
  return gillham_to_feet(id13_to_mode_a(ac13));
}

std::optional<std::int32_t> decode_ac12(std::uint16_t ac12) noexcept {
  ac12 &= 0x0FFF;
  if (ac12 == 0) return std::nullopt;
  // Re-insert a zero M bit at position 6 to form a 13-bit AC field.
  const auto ac13 = static_cast<std::uint16_t>(((ac12 & 0x0FC0u) << 1) | (ac12 & 0x003Fu));
  return decode_ac13(ac13);
}

}  // namespace adsb::modes
