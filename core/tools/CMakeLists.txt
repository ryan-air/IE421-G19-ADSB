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
