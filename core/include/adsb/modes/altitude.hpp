#pragma once

// Mode S altitude and identity (squawk) field decoding.
//
// 13-bit AC field (DF0/4/16/20), MSB first:
//     C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4
//     M = 1 -> metric altitude (not supported, essentially never transmitted)
//     Q = 1 -> 25 ft increments: N = the 11 remaining bits, alt = 25 N - 1000
//     Q = 0 -> Gillham (Gray) code in 100 ft increments
//
// 12-bit AC field (ADS-B airborne position, ME bits 9-20) is the same with the
// M bit removed.
//
// 13-bit ID field (DF5/21, ADS-B TC28): same bit positions as the AC field,
// with the M position carrying X (unused) and the Q position carrying D1.

#include <cstdint>
#include <optional>

namespace adsb::modes {

// Rearranges a 13-bit ID/AC field into the conventional Mode A layout
// 0xABCD, where each hex digit is a 3-bit octal digit (A4 A2 A1 etc.).
// For an ID field this *is* the squawk: e.g. squawk 7700 -> 0x7700.
[[nodiscard]] std::uint16_t id13_to_mode_a(std::uint16_t id13) noexcept;

// Decodes a Gillham-coded Mode A/C value (layout from id13_to_mode_a) to
// altitude in feet. Returns nullopt for codes that are not valid altitudes.
[[nodiscard]] std::optional<std::int32_t> gillham_to_feet(std::uint16_t mode_a) noexcept;

// Decodes a 13-bit AC field to feet. nullopt for "no altitude" (all zero),
// metric (M = 1), or an invalid Gillham code.
[[nodiscard]] std::optional<std::int32_t> decode_ac13(std::uint16_t ac13) noexcept;

// Decodes a 12-bit ADS-B altitude field to feet.
[[nodiscard]] std::optional<std::int32_t> decode_ac12(std::uint16_t ac12) noexcept;

// ---- Encoders (used by the synthetic traffic generator and tests) ---------

// Inverse of id13_to_mode_a: Mode A layout 0xABCD -> 13-bit ID field (X = 0).
[[nodiscard]] std::uint16_t mode_a_to_id13(std::uint16_t mode_a) noexcept;

// Encodes an altitude with the 25 ft Q-bit code (rounded to the nearest
// 25 ft). nullopt outside the representable range [-1000, 50175] ft.
[[nodiscard]] std::optional<std::uint16_t> encode_ac12(std::int32_t alt_ft) noexcept;
[[nodiscard]] std::optional<std::uint16_t> encode_ac13(std::int32_t alt_ft) noexcept;

}  // namespace adsb::modes
