add_executable(adsb-synth adsb_synth.cpp)
target_link_libraries(adsb-synth PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-synth)

add_executable(adsb-replay adsb_replay.cpp)
target_link_libraries(adsb-replay PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-replay)

add_executable(adsb-live adsb_live.cpp)
target_link_libraries(adsb-live PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-live)
