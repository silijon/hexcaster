cmake_minimum_required(VERSION 3.18)
project(HexCaster VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
add_compile_options(
  -Wall
  -Wextra
  -Wpedantic
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-ffast-math>
)

# Required for static libs linked into shared MODULE (LV2 plugin)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Options -- declared before dependencies.cmake so the dependency fetch
# can condition on them (e.g. FTXUI is only fetched when TUI is ON).
option(HEXCASTER_BUILD_LV2        "Build LV2 plugin"          ON)
option(HEXCASTER_BUILD_CLAP       "Build CLAP plugin"          ON)
option(HEXCASTER_BUILD_STANDALONE "Build standalone runtime"   ON)
option(HEXCASTER_BUILD_TESTS      "Build tests"                ON)
option(HEXCASTER_BUILD_TUI        "Build TUI for standalone"   ON)
option(HEXCASTER_DEBUG_CHANNELS   "CLAP: expose internal DSP signals on extra output channels" OFF)
option(HEXCASTER_OPTIMIZE_RPI5    "Use Cortex-A76 tuning for Raspberry Pi 5 ARM64 builds" ON)
option(HEXCASTER_ENABLE_LTO       "Enable interprocedural optimization in Release builds" ON)

if(HEXCASTER_OPTIMIZE_RPI5 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  add_compile_options($<$<CONFIG:Release>:-mcpu=cortex-a76>)
endif()

if(HEXCASTER_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT HEXCASTER_IPO_SUPPORTED OUTPUT HEXCASTER_IPO_ERROR)
  if(HEXCASTER_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(WARNING "LTO unavailable: ${HEXCASTER_IPO_ERROR}")
  endif()
endif()

# External dependencies
include(cmake/dependencies.cmake)

# Subdirectories
add_subdirectory(params)
add_subdirectory(dsp)

if(HEXCASTER_BUILD_STANDALONE)
  add_subdirectory(hosts/standalone)
endif()

if(HEXCASTER_BUILD_LV2)
  add_subdirectory(hosts/lv2)
endif()

if(HEXCASTER_BUILD_CLAP)
  add_subdirectory(hosts/clap)
endif()

if(HEXCASTER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
