# --- hexcaster_clap ---
# CLAP plugin host. Development and validation target for macOS (and Linux).
# Requires the CLAP SDK headers (fetched automatically via FetchContent).

add_library(hexcaster_clap MODULE
  hexcaster_clap.cpp
)

set_target_properties(hexcaster_clap PROPERTIES
  PREFIX ""
  OUTPUT_NAME "hexcaster"
  SUFFIX ".clap"
)

target_include_directories(hexcaster_clap
  PRIVATE
    ${clap_SOURCE_DIR}/include
)

target_link_libraries(hexcaster_clap
  PRIVATE
    hexcaster_pipeline   # brings in hexcaster_components and hexcaster_params transitively
)

if(HEXCASTER_DEBUG_CHANNELS)
  target_compile_definitions(hexcaster_clap PRIVATE HEXCASTER_DEBUG_CHANNELS=1)
endif()

# ---------------------------------------------------------------------------
# macOS: assemble the .clap bundle directory and auto-install
# ---------------------------------------------------------------------------

if(APPLE)
  set(CLAP_BUNDLE_DIR "${CMAKE_BINARY_DIR}/hexcaster.clap")

  # Assemble the bundle in the build directory after each build
  add_custom_command(TARGET hexcaster_clap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CLAP_BUNDLE_DIR}/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:hexcaster_clap>
        "${CLAP_BUNDLE_DIR}/Contents/MacOS/hexcaster"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
        "${CLAP_BUNDLE_DIR}/Contents/Info.plist"
    COMMENT "Assembling CLAP bundle: ${CLAP_BUNDLE_DIR}"
  )

  # Dev install: copy bundle to user CLAP directory so Reaper finds it
  set(CLAP_INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")

  add_custom_command(TARGET hexcaster_clap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLAP_INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CLAP_BUNDLE_DIR}"
        "${CLAP_INSTALL_DIR}/hexcaster.clap"
    COMMENT "Installing CLAP bundle to ${CLAP_INSTALL_DIR}/hexcaster.clap"
  )

else()
  # Linux: CLAP plugins are plain .clap shared objects in ~/.clap/
  set(CLAP_LINUX_INSTALL_DIR "$ENV{HOME}/.clap")

  add_custom_command(TARGET hexcaster_clap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLAP_LINUX_INSTALL_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:hexcaster_clap>
        "${CLAP_LINUX_INSTALL_DIR}/hexcaster.clap"
    COMMENT "Installing CLAP plugin to ${CLAP_LINUX_INSTALL_DIR}/hexcaster.clap"
  )
endif()
