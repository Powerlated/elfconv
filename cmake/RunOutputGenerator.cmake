if(NOT DEFINED ELFCONV_GENERATOR OR NOT EXISTS "${ELFCONV_GENERATOR}")
  message(FATAL_ERROR "State generator executable not found: ${ELFCONV_GENERATOR}")
endif()
if(NOT DEFINED ELFCONV_OUTPUT OR ELFCONV_OUTPUT STREQUAL "")
  message(FATAL_ERROR "Set ELFCONV_OUTPUT for state generation")
endif()

get_filename_component(output_directory "${ELFCONV_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
execute_process(
  COMMAND "${ELFCONV_GENERATOR}"
  OUTPUT_FILE "${ELFCONV_OUTPUT}"
  RESULT_VARIABLE result
  ERROR_VARIABLE error
)
if(NOT "${result}" STREQUAL "0")
  file(REMOVE "${ELFCONV_OUTPUT}")
  message(FATAL_ERROR "State generation failed (${result}): ${error}")
endif()
