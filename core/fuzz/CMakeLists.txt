# Fuzz targets. Each target defines LLVMFuzzerTestOneInput and is linked either
# with libFuzzer (ADSB_FUZZ_ENGINE=libfuzzer, needs LLVM clang) or with the
# portable standalone driver (replays a corpus plus seeded random mutations;
# works with Apple clang and GCC). Both are normally built with ASan + UBSan.

if(ADSB_FUZZ_ENGINE STREQUAL "libfuzzer")
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ADSB_FUZZ_ENGINE=libfuzzer requires clang")
  endif()
  # Coverage instrumentation for the code under test.
  target_compile_options(adsb_core PRIVATE -fsanitize=fuzzer-no-link)
endif()

function(adsb_add_fuzzer name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE adsb::core)
  adsb_apply_compiler_options(${name})
  if(ADSB_FUZZ_ENGINE STREQUAL "libfuzzer")
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(${name} PRIVATE standalone_driver.cpp)
  endif()
endfunction()

adsb_add_fuzzer(fuzz_beast)
adsb_add_fuzzer(fuzz_decoder)
adsb_add_fuzzer(fuzz_packet)
