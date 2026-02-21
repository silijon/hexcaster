# --- hexcaster_components ---
# Individual DSP stage implementations. Each stage is self-contained
# and implements the ProcessorStage interface.

add_library(hexcaster_components STATIC
  components/src/gain_stage.cpp
)

target_include_directories(hexcaster_components
  PUBLIC
    components/include
)

target_link_libraries(hexcaster_components
  PUBLIC
    hexcaster_params
)

# --- hexcaster_pipeline ---
# Signal flow composition layer. Wires stages and controllers together.
# Depends on components and params; hosts link against this.

add_library(hexcaster_pipeline STATIC
  pipeline/src/pipeline.cpp
)

target_include_directories(hexcaster_pipeline
  PUBLIC
    pipeline/include
)

target_link_libraries(hexcaster_pipeline
  PUBLIC
    hexcaster_components
    hexcaster_params
)
