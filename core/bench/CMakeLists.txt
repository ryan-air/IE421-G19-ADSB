add_executable(adsb_bench
  bench_spsc.cpp
  bench_histogram.cpp
  bench_modes.cpp
  bench_state.cpp
  bench_shm.cpp
  bench_demod.cpp
)
target_link_libraries(adsb_bench PRIVATE adsb::core benchmark::benchmark_main)
adsb_apply_compiler_options(adsb_bench)
