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
  src/feed/book.cpp
  src/feed/dst.cpp
  src/feed/snapshot.cpp
  src/feed/net.cpp
  src/feed/recovery.cpp
  src/feed/subscriber.cpp
  src/feed/shm_ring.cpp
  src/feed/shm_subscriber.cpp
  src/feed/uring_sender.cpp
  src/feed/xdp_sender.cpp
  src/feed/xdp_receiver.cpp
  src/record/recording.cpp
  src/sim/synth.cpp
  src/sim/iq.cpp
)
add_library(adsb::core ALIAS adsb_core)
target_include_directories(adsb_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(adsb_core PUBLIC cxx_std_20)
adsb_apply_compiler_options(adsb_core)

find_package(Threads REQUIRED)
target_link_libraries(adsb_core PUBLIC Threads::Threads)

# io_uring feed transport (§9.3): Linux with liburing. AUTO builds it when
# liburing is found; ON makes a missing liburing an error; OFF compiles the
# stub (UringSender::built() == false).
set(ADSB_IO_URING AUTO CACHE STRING "io_uring transport: AUTO, ON, OFF")
set(ADSB_HAVE_IO_URING 0)
if(NOT ADSB_IO_URING STREQUAL "OFF" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing>=2.2)
  endif()
  if(LIBURING_FOUND)
    set(ADSB_HAVE_IO_URING 1)
    target_link_libraries(adsb_core PRIVATE PkgConfig::LIBURING)
  endif()
endif()
if(ADSB_IO_URING STREQUAL "ON" AND NOT ADSB_HAVE_IO_URING)
  message(FATAL_ERROR "ADSB_IO_URING=ON but liburing (>= 2.2) was not found (apt install liburing-dev pkg-config)")
endif()
target_compile_definitions(adsb_core PUBLIC ADSB_HAVE_IO_URING=${ADSB_HAVE_IO_URING})
message(STATUS "adsb: io_uring transport ${ADSB_HAVE_IO_URING} (ADSB_IO_URING=${ADSB_IO_URING})")

# AF_XDP feed transport (§9.3): Linux kernel headers only, no libbpf/libxdp
# (the subscriber's XDP program is built as eBPF instructions and loaded with
# the bpf() syscall). AUTO builds it when <linux/if_xdp.h> and <linux/bpf.h>
# have what it uses (kernel 5.9+ headers: XSKMAP, BPF links); ON makes that
# an error; OFF compiles the stubs (XdpSender/XdpReceiver::built() == false).
set(ADSB_AF_XDP AUTO CACHE STRING "AF_XDP transport: AUTO, ON, OFF")
set(ADSB_HAVE_AF_XDP 0)
if(NOT ADSB_AF_XDP STREQUAL "OFF" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles([=[
    #include <linux/bpf.h>
    #include <linux/if_link.h>
    #include <linux/if_xdp.h>
    int main() { xdp_statistics s{}; xdp_options o{}; bpf_attr a{}; a.link_create.target_ifindex = 1;
                 return XDP_USE_NEED_WAKEUP + XDP_OPTIONS_ZEROCOPY + XDP_RING_NEED_WAKEUP
                 + static_cast<int>(s.tx_invalid_descs + s.rx_ring_full + o.flags) + BPF_MAP_TYPE_XSKMAP
                 + BPF_LINK_CREATE + BPF_XDP + XDP_FLAGS_DRV_MODE + XDP_FLAGS_SKB_MODE
                 + static_cast<int>(a.link_create.target_ifindex); }
  ]=] ADSB_IF_XDP_HEADER_OK)
  if(ADSB_IF_XDP_HEADER_OK)
    set(ADSB_HAVE_AF_XDP 1)
  endif()
endif()
if(ADSB_AF_XDP STREQUAL "ON" AND NOT ADSB_HAVE_AF_XDP)
  message(FATAL_ERROR "ADSB_AF_XDP=ON but <linux/if_xdp.h> / <linux/bpf.h> (kernel 5.9+ headers) were not found")
endif()
target_compile_definitions(adsb_core PUBLIC ADSB_HAVE_AF_XDP=${ADSB_HAVE_AF_XDP})
message(STATUS "adsb: AF_XDP transport ${ADSB_HAVE_AF_XDP} (ADSB_AF_XDP=${ADSB_AF_XDP})")
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
