# Third-party test and benchmark frameworks, pinned by version and SHA-256.
# They are only fetched when tests or benchmarks are enabled; the core library
# itself has no third-party dependencies. They are always built from source so
# sanitizer builds instrument them too.

include(FetchContent)

if(ADSB_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL      https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)
endif()

if(ADSB_BUILD_BENCH)
  set(BENCHMARK_ENABLE_TESTING        OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS    OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL        OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS          OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR         OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    URL      https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(benchmark)
endif()
