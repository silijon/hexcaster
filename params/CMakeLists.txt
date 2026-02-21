# --- hexcaster_params ---
# Parameter system: atomic parameter registry, per-sample smoothing,
# and MIDI mapping (stub). Independent of DSP and host layers.

add_library(hexcaster_params STATIC
  src/param_registry.cpp
  src/param_smoother.cpp
)

target_include_directories(hexcaster_params
  PUBLIC
    include
)
