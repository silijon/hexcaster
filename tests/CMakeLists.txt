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
