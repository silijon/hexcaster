# --- hexcaster_standalone ---
# Headless runtime host for Raspberry Pi / Linux.
# Thin wrapper over hexcaster_pipeline + hexcaster_params.
# Requires ALSA (libasound2-dev / alsa-lib-devel).

find_package(ALSA REQUIRED)
find_package(Threads REQUIRED)

set(STANDALONE_SOURCES
  main.cpp
  alsa_audio_engine.cpp
  midi_input.cpp
)

# TUI sources -- only compiled when HEXCASTER_BUILD_TUI is ON
if(HEXCASTER_BUILD_TUI AND HEXCASTER_BUILD_STANDALONE)
  list(APPEND STANDALONE_SOURCES
    tui/tui.cpp
    tui/screens/main_screen.cpp
    tui/screens/eq_screen.cpp
    tui/screens/bloom_screen.cpp
  )
endif()

add_executable(hexcaster_standalone ${STANDALONE_SOURCES})

target_include_directories(hexcaster_standalone
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}       # for audio_engine.h, alsa_audio_engine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tui   # for tui headers
)

target_link_libraries(hexcaster_standalone
  PRIVATE
    hexcaster_pipeline
    hexcaster_params
    ALSA::ALSA
    Threads::Threads
)

if(HEXCASTER_BUILD_TUI AND HEXCASTER_BUILD_STANDALONE)
  target_link_libraries(hexcaster_standalone
    PRIVATE
      ftxui::component
      ftxui::dom
      ftxui::screen
  )
  target_compile_definitions(hexcaster_standalone PRIVATE HEXCASTER_TUI_ENABLED=1)
endif()
