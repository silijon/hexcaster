# --- hexcaster_lv2 ---
# LV2 plugin host. Development and validation target.
# Requires LV2 headers (liblv2-dev or equivalent).

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LV2 QUIET lv2)
endif()

if(NOT LV2_FOUND)
  # Fallback: try to locate lv2.h directly
  find_path(LV2_INCLUDE_DIR lv2.h
    PATHS /usr/include /usr/local/include
    PATH_SUFFIXES lv2
  )
  if(LV2_INCLUDE_DIR)
    set(LV2_FOUND TRUE)
    set(LV2_INCLUDE_DIRS ${LV2_INCLUDE_DIR})
  endif()
endif()

if(NOT LV2_FOUND)
  message(WARNING "LV2 headers not found -- skipping hexcaster_lv2 build. "
                  "Install liblv2-dev (or equivalent) to build the LV2 plugin.")
  return()
endif()

add_library(hexcaster_lv2 MODULE
  hexcaster_lv2.cpp
)

set_target_properties(hexcaster_lv2 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "hexcaster"
)

target_include_directories(hexcaster_lv2
  PRIVATE
    ${LV2_INCLUDE_DIRS}
)

target_link_libraries(hexcaster_lv2
  PRIVATE
    hexcaster_pipeline
    hexcaster_params
)

# Bundle destination: ~/.lv2/hexcaster.lv2/
set(LV2_BUNDLE_DIR "$ENV{HOME}/.lv2/hexcaster.lv2")

# Post-build: copy the .so and TTL files into the bundle directory
# so the plugin is immediately available to hosts after every build --
# no manual `cmake --install` required during development.
add_custom_command(TARGET hexcaster_lv2 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${LV2_BUNDLE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE:hexcaster_lv2>
      ${LV2_BUNDLE_DIR}/hexcaster.so
  COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_CURRENT_SOURCE_DIR}/manifest.ttl
      ${CMAKE_CURRENT_SOURCE_DIR}/hexcaster.ttl
      ${LV2_BUNDLE_DIR}
  COMMENT "Installing LV2 bundle to ${LV2_BUNDLE_DIR}"
)

# `cmake --install` target (optional, same result)
install(TARGETS hexcaster_lv2
  DESTINATION ${LV2_BUNDLE_DIR}
)

install(FILES
  manifest.ttl
  hexcaster.ttl
  DESTINATION ${LV2_BUNDLE_DIR}
)
