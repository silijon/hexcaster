# --- hexcaster_tests ---
# No external test framework -- plain main() with assertions.

add_executable(hexcaster_tests
  test_passthrough.cpp
)

target_link_libraries(hexcaster_tests
  PRIVATE
    hexcaster_pipeline
    hexcaster_params
)

add_test(NAME passthrough COMMAND hexcaster_tests)

add_executable(hexcaster_input_gain_tests test_input_gain.cpp)
target_link_libraries(hexcaster_input_gain_tests
  PRIVATE hexcaster_params NeuralAudio)
add_test(NAME input_gain_calibration COMMAND hexcaster_input_gain_tests)

add_executable(hexcaster_nam_a2_tests test_nam_a2.cpp)
target_link_libraries(hexcaster_nam_a2_tests PRIVATE hexcaster_components)

add_test(NAME nam_a2
  COMMAND hexcaster_nam_a2_tests
    ${neuralaudio_SOURCE_DIR}/Utils/Models/BossWN-a2.nam
    ${neuralaudio_SOURCE_DIR}/Utils/Models/BossWN-standard.nam
)
