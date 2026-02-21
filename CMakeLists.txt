cmake_minimum_required(VERSION 3.18)
project(HexCaster VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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

# Optional: flush denormals to zero (x86/ARM)
add_compile_options(-ffast-math)

# Required for static libs linked into shared MODULE (LV2 plugin)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Options
option(HEXCASTER_BUILD_LV2        "Build LV2 plugin"          ON)
option(HEXCASTER_BUILD_STANDALONE "Build standalone runtime"   ON)
option(HEXCASTER_BUILD_TESTS      "Build tests"                ON)

# Subdirectories
add_subdirectory(params)
add_subdirectory(dsp)

if(HEXCASTER_BUILD_STANDALONE)
  add_subdirectory(hosts/standalone)
endif()

if(HEXCASTER_BUILD_LV2)
  add_subdirectory(hosts/lv2)
endif()

if(HEXCASTER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
