# --- hexcaster_standalone ---
# Headless runtime host for Raspberry Pi / Linux.
# Thin wrapper over hexcaster_pipeline + hexcaster_params.
# Requires ALSA (libasound2-dev / alsa-lib-devel).

find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

add_executable(hexcaster_standalone
  main.cpp
  alsa_audio_engine.cpp
  midi_input.cpp
)

target_include_directories(hexcaster_standalone
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}   # for audio_engine.h, alsa_audio_engine.h
)

target_link_libraries(hexcaster_standalone
  PRIVATE
    hexcaster_pipeline
    hexcaster_params
    ALSA::ALSA
    Threads::Threads
)
