add_executable(adsb-synth adsb_synth.cpp)
target_link_libraries(adsb-synth PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-synth)

add_executable(adsb-replay adsb_replay.cpp)
target_link_libraries(adsb-replay PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-replay)

add_executable(adsb-live adsb_live.cpp)
target_link_libraries(adsb-live PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-live)

add_executable(adsb-publish adsb_publish.cpp)
target_link_libraries(adsb-publish PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-publish)

add_executable(adsb-feed-cli adsb_feed_cli.cpp)
target_link_libraries(adsb-feed-cli PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-feed-cli)

add_executable(adsb-clock adsb_clock.cpp)
target_link_libraries(adsb-clock PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-clock)

add_executable(adsb-beast-serve adsb_beast_serve.cpp)
target_link_libraries(adsb-beast-serve PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-beast-serve)

add_executable(adsb-shm-cli adsb_shm_cli.cpp)
target_link_libraries(adsb-shm-cli PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-shm-cli)

add_executable(adsb-shm-bench adsb_shm_bench.cpp)
target_link_libraries(adsb-shm-bench PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-shm-bench)

add_executable(adsb-ring-stress adsb_ring_stress.cpp)
target_link_libraries(adsb-ring-stress PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-ring-stress)

add_executable(adsb-tx-bench adsb_tx_bench.cpp)
target_link_libraries(adsb-tx-bench PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-tx-bench)

add_executable(adsb-dst adsb_dst.cpp)
target_link_libraries(adsb-dst PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-dst)

add_executable(adsb-iqsynth adsb_iqsynth.cpp)
target_link_libraries(adsb-iqsynth PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-iqsynth)

add_executable(adsb-demod adsb_demod.cpp)
target_link_libraries(adsb-demod PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-demod)
