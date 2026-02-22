include(FetchContent)

# ---------------------------------------------------------------------------
# NeuralAudio
# NAM-compatible neural amp model inference engine.
# Pinned to a specific commit on the release branch for reproducibility.
#
# We use FetchContent_Populate (not MakeAvailable) so we can point directly
# at NeuralAudio/NeuralAudio/ and skip the NeuralAudioCAPI shared library
# target, which we don't need.
# ---------------------------------------------------------------------------

# NeuralAudio build options -- must be set before add_subdirectory
set(BUILD_STATIC_RTNEURAL ON  CACHE BOOL   "Build static RTNeural models" FORCE)
set(BUILD_NAMCORE         OFF CACHE BOOL   "Build NAM Core (benchmarking only)" FORCE)
set(BUILD_UTILS           OFF CACHE BOOL   "Build NeuralAudio utils" FORCE)
set(WAVENET_FRAMES        "128" CACHE STRING "WaveNet frame size (match max buffer)" FORCE)

FetchContent_Declare(
  NeuralAudio
  GIT_REPOSITORY https://github.com/mikeoliphant/NeuralAudio.git
  GIT_TAG        1eab4645f7073e752314b33946b69bfe3fbc01f9
  GIT_SUBMODULES_RECURSE ON
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
endif()
