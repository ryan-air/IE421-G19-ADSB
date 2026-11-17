add_executable(adsb-synth adsb_synth.cpp)
target_link_libraries(adsb-synth PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-synth)

add_executable(adsb-replay adsb_replay.cpp)
target_link_libraries(adsb-replay PRIVATE adsb::core)
adsb_apply_compiler_options(adsb-replay)
