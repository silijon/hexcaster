# --- hexcaster_standalone ---
# Headless runtime host for Raspberry Pi / Linux.
# Thin wrapper over hexcaster_pipeline + hexcaster_params.

add_executable(hexcaster_standalone
  main.cpp
)

target_link_libraries(hexcaster_standalone
  PRIVATE
    hexcaster_pipeline
    hexcaster_params
)
