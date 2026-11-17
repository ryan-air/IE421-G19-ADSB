include(GoogleTest)

set(ADSB_TEST_SOURCES
  test_spsc_ring.cpp
  test_histogram.cpp
  test_clock.cpp
  test_crc24.cpp
  test_altitude.cpp
  test_cpr.cpp
  test_decoder.cpp
  test_beast.cpp
  test_protocol.cpp
  test_hash.cpp
  test_encoder.cpp
  test_recording.cpp
  test_track_store.cpp
  test_track_engine.cpp
  test_synth_replay.cpp
)

add_executable(adsb_tests ${ADSB_TEST_SOURCES})
target_link_libraries(adsb_tests PRIVATE adsb::core GTest::gtest_main)
adsb_apply_compiler_options(adsb_tests)
target_compile_definitions(adsb_tests PRIVATE
  ADSB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../protocol/golden")

# Sanitizer runs are slower; give discovery and the stress tests extra time.
gtest_discover_tests(adsb_tests
  DISCOVERY_TIMEOUT 60
  PROPERTIES TIMEOUT 300)
