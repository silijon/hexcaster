if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE)
  message(FATAL_ERROR "apply_patch_once.cmake requires SOURCE_DIR and PATCH_FILE")
endif()

# FetchContent may execute PATCH_COMMAND again on a later configure. First
# detect the already-applied state so a populated dependency remains reusable.
execute_process(
  COMMAND git apply --reverse --check --recount --whitespace=nowarn "${PATCH_FILE}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE reverse_check_result
  OUTPUT_QUIET
  ERROR_QUIET
)
if(reverse_check_result EQUAL 0)
  message(STATUS "Dependency patch already applied: ${PATCH_FILE}")
  return()
endif()

execute_process(
  COMMAND git apply --check --recount --whitespace=nowarn "${PATCH_FILE}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE apply_check_result
  OUTPUT_VARIABLE apply_check_output
  ERROR_VARIABLE apply_check_error
)
if(NOT apply_check_result EQUAL 0)
  message(FATAL_ERROR
    "Dependency patch is neither applicable nor already applied.\n"
    "Source: ${SOURCE_DIR}\nPatch: ${PATCH_FILE}\n"
    "${apply_check_output}${apply_check_error}")
endif()

execute_process(
  COMMAND git apply --recount --whitespace=nowarn "${PATCH_FILE}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE apply_result
  OUTPUT_VARIABLE apply_output
  ERROR_VARIABLE apply_error
)
if(NOT apply_result EQUAL 0)
  message(FATAL_ERROR
    "Failed to apply dependency patch.\n${apply_output}${apply_error}")
endif()

message(STATUS "Applied dependency patch: ${PATCH_FILE}")
