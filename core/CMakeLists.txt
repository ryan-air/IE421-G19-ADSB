cmake_minimum_required(VERSION 3.24)

project(adsb_core
  VERSION 0.1.0
  DESCRIPTION "Low-latency ADS-B / Mode S pipeline core"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ADSB_BUILD_TESTS   "Build unit tests"                        ON)
option(ADSB_BUILD_BENCH   "Build Google Benchmark microbenchmarks"  OFF)
option(ADSB_BUILD_FUZZ    "Build fuzz targets"                      OFF)
option(ADSB_WERROR        "Treat compiler warnings as errors"       OFF)
option(ADSB_NATIVE        "Optimize for the build machine's CPU"    OFF)
option(ADSB_LTO           "Enable link-time optimization"           OFF)
option(ADSB_NAIVE         "Build the naive baseline (mutex queues, std::unordered_map)" OFF)
option(ADSB_BUILD_TOOLS   "Build command-line tools (adsb-synth, adsb-replay)" ON)
set(ADSB_SANITIZE "" CACHE STRING
  "Semicolon-separated sanitizers: address;undefined | thread")
set(ADSB_FUZZ_ENGINE "standalone" CACHE STRING
  "Fuzz engine: 'libfuzzer' (clang with -fsanitize=fuzzer) or 'standalone' (portable driver)")
set_property(CACHE ADSB_FUZZ_ENGINE PROPERTY STRINGS libfuzzer standalone)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Sanitizers)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(adsb_core STATIC
  src/common/clock.cpp
  src/common/hash.cpp
  src/common/thread.cpp
  src/modes/crc24.cpp
  src/modes/altitude.cpp
  src/modes/cpr.cpp
  src/modes/decoder.cpp
  src/modes/encoder.cpp
  src/ingest/beast_tcp.cpp
  src/record/recording.cpp
  src/sim/synth.cpp
)
add_library(adsb::core ALIAS adsb_core)
target_include_directories(adsb_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(adsb_core PUBLIC cxx_std_20)
adsb_apply_compiler_options(adsb_core)

find_package(Threads REQUIRED)
target_link_libraries(adsb_core PUBLIC Threads::Threads)
if(ADSB_NAIVE)
  target_compile_definitions(adsb_core PUBLIC ADSB_NAIVE=1)
  message(STATUS "adsb: NAIVE baseline build (mutex queues, std::unordered_map)")
endif()

# ---------------------------------------------------------------------------
# Tests, benchmarks, fuzzers
# ---------------------------------------------------------------------------
if(ADSB_BUILD_TESTS OR ADSB_BUILD_BENCH)
  include(Dependencies)
endif()

if(ADSB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ADSB_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(ADSB_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()

if(ADSB_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
