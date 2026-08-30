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

set_target_properties(hexcaster_standalone PROPERTIES
  OUTPUT_NAME hexcaster
)

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

# Raspberry Pi production installation. The generated service uses absolute
# paths derived from CMAKE_INSTALL_PREFIX; configure with /usr on the Pi.
set(HEXCASTER_SYSTEMD_UNIT_DIR "lib/systemd/system"
    CACHE PATH "systemd system unit installation directory")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/deployment/hexcaster.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/hexcaster.service
  @ONLY
)

install(TARGETS hexcaster_standalone
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT hexcaster-runtime
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hexcaster.service
  DESTINATION ${HEXCASTER_SYSTEMD_UNIT_DIR}
  COMPONENT hexcaster-runtime
)
install(FILES deployment/hexcaster.sysusers
  DESTINATION lib/sysusers.d
  RENAME hexcaster.conf
  COMPONENT hexcaster-runtime
)
install(FILES deployment/hexcaster.env.example
  DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/hexcaster
  COMPONENT hexcaster-runtime
)
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/hexcaster/models
  COMPONENT hexcaster-runtime
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_custom_target(install-pi
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
            --component hexcaster-runtime
    COMMAND systemd-sysusers
            ${CMAKE_CURRENT_SOURCE_DIR}/deployment/hexcaster.sysusers
    COMMAND systemctl daemon-reload
    COMMAND systemctl enable hexcaster.service
    COMMAND ${CMAKE_COMMAND} -E echo
            "HexCaster installed and enabled. Configure ${CMAKE_INSTALL_FULL_SYSCONFDIR}/hexcaster/hexcaster.env, install a model, then run: systemctl start hexcaster"
    DEPENDS hexcaster_standalone
    USES_TERMINAL
    COMMENT "Installing and enabling the HexCaster Pi system service"
  )
endif()
