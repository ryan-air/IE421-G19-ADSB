# Warning, optimization, and code-generation flags shared by every target.

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

set(ADSB_WARNING_FLAGS
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wold-style-cast
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wnull-dereference
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
)

# Pick the right "optimize for this machine" flag: x86 uses -march=native,
# AArch64 compilers (Apple clang in particular) prefer -mcpu=native.
set(ADSB_NATIVE_FLAG "")
if(ADSB_NATIVE)
  check_cxx_compiler_flag("-march=native" ADSB_HAS_MARCH_NATIVE)
  check_cxx_compiler_flag("-mcpu=native"  ADSB_HAS_MCPU_NATIVE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND ADSB_HAS_MCPU_NATIVE)
    set(ADSB_NATIVE_FLAG "-mcpu=native")
  elseif(ADSB_HAS_MARCH_NATIVE)
    set(ADSB_NATIVE_FLAG "-march=native")
  elseif(ADSB_HAS_MCPU_NATIVE)
    set(ADSB_NATIVE_FLAG "-mcpu=native")
  endif()
endif()

if(ADSB_LTO)
  check_ipo_supported(RESULT ADSB_IPO_OK OUTPUT ADSB_IPO_MSG LANGUAGES CXX)
  if(NOT ADSB_IPO_OK)
    message(WARNING "LTO requested but not supported: ${ADSB_IPO_MSG}")
  endif()
endif()

function(adsb_apply_compiler_options target)
  target_compile_options(${target} PRIVATE ${ADSB_WARNING_FLAGS})
  # No fused multiply-add contraction: keeps floating-point results (CPR
  # decoding, unit conversions) identical across optimization levels and
  # x86/ARM, which the replay determinism checks rely on.
  target_compile_options(${target} PRIVATE -ffp-contract=off)
  if(ADSB_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
  if(ADSB_NATIVE_FLAG)
    target_compile_options(${target} PRIVATE ${ADSB_NATIVE_FLAG})
  endif()
  if(ADSB_LTO AND ADSB_IPO_OK)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endfunction()
