include(FetchContent)

# ---------------------------------------------------------------------------
# CLAP SDK
# Header-only C plugin API. MIT license. Used by the CLAP plugin host.
# Only fetched when HEXCASTER_BUILD_CLAP is ON.
# ---------------------------------------------------------------------------

if(HEXCASTER_BUILD_CLAP)
  FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.6
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(clap)
endif()

# ---------------------------------------------------------------------------
# FTXUI
# C++ functional terminal UI library. Zero external dependencies, MIT license.
# Used by the optional standalone TUI mode (--tui flag).
# Only fetched when HEXCASTER_BUILD_TUI is ON.
# ---------------------------------------------------------------------------

if(HEXCASTER_BUILD_TUI)
  FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v6.1.9
    GIT_SHALLOW    TRUE
  )
  # Disable FTXUI's own examples and tests to keep the build lean
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(ftxui)
endif()

# ---------------------------------------------------------------------------
# NeuralAudio
# NAM-compatible neural amp model inference engine.
# Pinned to a specific commit on the release branch for reproducibility.
#
# We use FetchContent_Populate (not MakeAvailable) so we can point directly
# at NeuralAudio/NeuralAudio/ and skip the NeuralAudioCAPI shared library
# target, which we don't need.
# ---------------------------------------------------------------------------

# A2-only NeuralAudio build. Standard A2 Lite/Full models use NeuralAudio's
# native static kernels; NAMCore is retained solely as the compatibility
# fallback for non-standard A2 files. RTNeural is patched out entirely.
set(BUILD_RTNEURAL              OFF CACHE BOOL "Build RTNeural support" FORCE)
set(BUILD_STATIC_RTNEURAL       OFF CACHE BOOL "Build static RTNeural models" FORCE)
set(BUILD_NAMCORE               ON  CACHE BOOL "Build NAMCore A2 fallback" FORCE)
set(BUILD_INTERNAL_STATIC_WAVENET OFF CACHE BOOL "Build legacy A1 kernels" FORCE)
set(BUILD_STATIC_INTERNAL_NAMA2 ON  CACHE BOOL "Build native static A2 kernels" FORCE)
set(BUILD_INTERNAL_STATIC_LSTM  OFF CACHE BOOL "Build internal LSTM kernels" FORCE)
set(NAM_ENABLE_A2_FAST          ON  CACHE BOOL "Enable NAMCore A2 fast path" FORCE)
set(BUILD_UTILS           OFF CACHE BOOL   "Build NeuralAudio utils" FORCE)
set(WAVENET_FRAMES        "128" CACHE STRING "Maximum WaveNet processing block" FORCE)

# NeuralAudio historically sourced these headers from RTNeural. They are
# explicit dependencies in the RTNeural-free build.
FetchContent_Declare(
  Eigen3
  GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
  # NeuralAudio uses Eigen's indexed-view lastN API, which is newer than 3.4.0.
  GIT_TAG        f96618cd2cfa983e5390c666e4fca6cebc462e26
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Eigen3 nlohmann_json)

FetchContent_Declare(
  NeuralAudio
  GIT_REPOSITORY https://github.com/mikeoliphant/NeuralAudio.git
  GIT_TAG        e59cd5d473d5b5772c69e755d7c5bc1007cff9ab
  GIT_SUBMODULES deps/NeuralAmpModelerCore deps/math_approx
  GIT_SUBMODULES_RECURSE ON
  # CMake 3.31 can rerun this step for an already-populated dependency, so the
  # patch driver must tolerate a patch that is already present.
  PATCH_COMMAND
    ${CMAKE_COMMAND}
      -DSOURCE_DIR=<SOURCE_DIR>
      -DPATCH_FILE=${CMAKE_CURRENT_LIST_DIR}/patches/neuralaudio-a2-only.patch
      -P ${CMAKE_CURRENT_LIST_DIR}/apply_patch_once.cmake
)

FetchContent_GetProperties(NeuralAudio)
if(NOT neuralaudio_POPULATED)
  message(STATUS "Fetching NeuralAudio...")

  # Suppress CMP0169 deprecation for FetchContent_Populate (direct call).
  # We use the direct form intentionally to skip NeuralAudioCAPI without
  # polluting the source tree. Revisit when CMake minimum is bumped to 3.28+
  # (which adds SOURCE_SUBDIR to FetchContent_Declare).
  cmake_policy(PUSH)
  cmake_policy(SET CMP0169 OLD)
  FetchContent_Populate(NeuralAudio)
  cmake_policy(POP)

  # Add only the core NeuralAudio library, skipping NeuralAudioCAPI
  add_subdirectory(
    ${neuralaudio_SOURCE_DIR}/NeuralAudio
    ${neuralaudio_BINARY_DIR}/NeuralAudio
  )

  target_link_libraries(NeuralAudio PUBLIC Eigen3::Eigen nlohmann_json::nlohmann_json)
  target_include_directories(NeuralAudio SYSTEM PUBLIC
    ${nlohmann_json_SOURCE_DIR}/single_include/nlohmann
  )
endif()
