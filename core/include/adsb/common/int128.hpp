#pragma once

namespace adsb {

// GCC/Clang 128-bit unsigned integer; __extension__ keeps -Wpedantic quiet.
__extension__ typedef unsigned __int128 uint128;

}  // namespace adsb
