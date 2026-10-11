set(glibc_stdio_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(glibc_stdio_source "${CMAKE_CURRENT_LIST_DIR}")
set(glibc_stdio_support "${glibc_stdio_root}/support")
set(glibc_stdio_compat "${CMAKE_CURRENT_LIST_DIR}/compat")

# The full support tree includes test programs and glibc-build-only helpers.
file(GLOB glibc_stdio_support_sources CONFIGURE_DEPENDS "${glibc_stdio_support}/*.c")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/(README-testing|echo-container|shell-container|true-container|test-container|test-driver|test-run-command|links-dso-program-c|support_quote_blob_main|support_test_compare_string_main)\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/tst-[^/]+\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/(dtotimespec(-time64)?|timespec(-add(-time64)?|-sub(-time64)?|-time64)?)\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/support_capture_subprocess(_check)?\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/(resolv_test|support_descriptors)\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/(support_process_state|support_path_support_time64)\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/support_small_stack_thread_attribute\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/(support_socket_so_timestamp_time64|support_stack_alloc)\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/xclock_(get|set)time_time64\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/xclone\\.c$")
list(FILTER glibc_stdio_support_sources EXCLUDE REGEX "/xreadlink\\.c$")
add_library(glibc_stdio_support STATIC ${glibc_stdio_support_sources})
target_include_directories(glibc_stdio_support PUBLIC "${glibc_stdio_root}" "${glibc_stdio_support}")
target_compile_definitions(glibc_stdio_support PRIVATE
  "SRCDIR_PATH=\"${glibc_stdio_source}\""
  "OBJDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "OBJDIR_ELF_LDSO_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "INSTDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "LIBDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "BINDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "SBINDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "SLIBDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "ROOTSBINDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "COMPLOCALEDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
  "SYSCONFDIR_PATH=\"${CMAKE_CURRENT_BINARY_DIR}\""
)
target_compile_definitions(glibc_stdio_support PRIVATE TIMEOUTFACTOR=1)
target_compile_options(glibc_stdio_support PRIVATE -D_GNU_SOURCE -m32 -pthread)
target_compile_options(glibc_stdio_support PRIVATE -include limits.h)
add_dependencies(elflift glibc_stdio_support)

# Register the Makefile's `tests` list. Container/internal categories require
# glibc's isolated harness; for example, tst-popen3 unlinks `_PATH_BSHELL`.
file(STRINGS "${glibc_stdio_source}/Makefile" glibc_stdio_makefile_lines)
set(glibc_stdio_test_names)
foreach(line IN LISTS glibc_stdio_makefile_lines)
  if("${line}" MATCHES "^tests[ \t]*([:+]?=)")
    string(REGEX REPLACE "^[^=]*=" "" entry "${line}")
    string(REGEX REPLACE "#.*$" "" entry "${entry}")
    string(REPLACE ";" " " entry "${entry}")
    string(REGEX MATCHALL "[A-Za-z0-9_-]+" names "${entry}")
    list(APPEND glibc_stdio_test_names ${names})
  endif()
endforeach()

# These Makefile test sources are standalone and produce observable output.
list(APPEND glibc_stdio_test_names
  tst-printf
  tst-printfsz-islongdouble
  tst-unbputc)
list(REMOVE_DUPLICATES glibc_stdio_test_names)

find_program(ELFCONV_GLIBC_BASH bash REQUIRED)
set(glibc_stdio_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${glibc_stdio_generated_dir}")
execute_process(
  COMMAND "${ELFCONV_GLIBC_BASH}" "${glibc_stdio_source}/tst-printf-bz18872.sh"
  OUTPUT_FILE "${glibc_stdio_generated_dir}/tst-printf-bz18872.c"
  RESULT_VARIABLE glibc_stdio_generate_result)
if(NOT glibc_stdio_generate_result EQUAL 0)
  message(FATAL_ERROR "Could not generate glibc stdio test tst-printf-bz18872.c")
endif()

set(glibc_stdio_common_args
  --lifter "$<TARGET_FILE:elflift>"
  --emcc "${ELFCONV_EMCC}"
  --cc "${ELFCONV_I386_CC}"
  --objcopy "${ELFCONV_I386_OBJCOPY}"
  --node "${ELFCONV_I386_NODE}"
  "--compile-flag=-I${glibc_stdio_compat}"
  "--compile-flag=-DOBJPFX=\"\""
  "--compile-flag=-D_GNU_SOURCE"
  "--compile-flag=-I${glibc_stdio_root}"
  "--compile-flag=-I${glibc_stdio_support}"
  "--compile-flag=-I${glibc_stdio_source}"
  --compare-stderr
  --link-flag "$<TARGET_FILE:glibc_stdio_support>"
)

set(glibc_stdio_registered_tests)
foreach(test_name IN LISTS glibc_stdio_test_names)
  if(test_name STREQUAL "tst-printf-bz18872")
    set(test_source "${glibc_stdio_generated_dir}/${test_name}.c")
  else()
    set(test_source "${glibc_stdio_source}/${test_name}.c")
  endif()
  if(NOT EXISTS "${test_source}")
    message(FATAL_ERROR "Missing upstream glibc stdio test source: ${test_source}")
  endif()
  set(test_stdin_args)
  if(EXISTS "${glibc_stdio_source}/${test_name}.input")
    list(APPEND test_stdin_args --stdin-file "${glibc_stdio_source}/${test_name}.input")
  endif()
  add_test(NAME "i386_glibc_stdio_${test_name}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/test/elfconv/run_i386.py"
      ${glibc_stdio_common_args}
      --source "${test_source}"
      ${test_stdin_args}
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${test_name}")
  set_tests_properties("i386_glibc_stdio_${test_name}" PROPERTIES
    LABELS "i386;regression;glibc;stdio" TIMEOUT 1000)
  list(APPEND glibc_stdio_registered_tests "i386_glibc_stdio_${test_name}")
endforeach()


file(GLOB glibc_printf_format_sources "${glibc_stdio_source}/tst-printf-format-*.c")
foreach(test_source IN LISTS glibc_printf_format_sources)
  get_filename_component(test_name "${test_source}" NAME_WE)
  if(test_name MATCHES "^tst-printf-format-(p|as|d|f|s|sn|v|vas|vd|vf|vs|vsn)-(c|char|int|llong|long|s|short|uchar|uint|ullong|ulong|ushort|double|ldouble)(-(E|e|F|f|G|g))?$")
    add_test(NAME "i386_glibc_stdio_${test_name}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/test/elfconv/run_i386.py"
        ${glibc_stdio_common_args}
        --source "${test_source}"
        --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${test_name}")
    set_tests_properties("i386_glibc_stdio_${test_name}" PROPERTIES
      LABELS "i386;regression;glibc;stdio;printf" TIMEOUT 1000)
    list(APPEND glibc_stdio_registered_tests "i386_glibc_stdio_${test_name}")
  endif()
endforeach()

file(GLOB glibc_scanf_format_sources "${glibc_stdio_source}/tst-scanf-format-*.c")
foreach(test_source IN LISTS glibc_scanf_format_sources)
  get_filename_component(test_name "${test_source}" NAME_WE)
  if(NOT test_name MATCHES "^tst-scanf-format-(s|f|ss|v|vf|vs)-(c|char|llong|short|uchar|ullong|ushort)$")
    continue()
  endif()
  string(REGEX REPLACE "^tst-scanf-format-(s|f|ss|v|vf|vs)-" "" input_kind "${test_name}")
  file(GLOB input_files "${glibc_stdio_source}/tst-scanf-format-${input_kind}-*.input")
  if(NOT input_files)
    message(FATAL_ERROR "Missing upstream glibc scanf input files for ${test_name}")
  endif()
  foreach(input_file IN LISTS input_files)
    get_filename_component(input_name "${input_file}" NAME_WE)
    add_test(NAME "i386_glibc_stdio_${test_name}_${input_name}"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/test/elfconv/run_i386.py"
        ${glibc_stdio_common_args}
        --source "${test_source}"
        --stdin-file "${input_file}"
        --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${test_name}/${input_name}")
    set_tests_properties("i386_glibc_stdio_${test_name}_${input_name}" PROPERTIES
      LABELS "i386;regression;glibc;stdio;scanf" TIMEOUT 1000)
    list(APPEND glibc_stdio_registered_tests "i386_glibc_stdio_${test_name}_${input_name}")
  endforeach()
endforeach()

set(ELFCONV_GLIBC_STDIO_LOCALE_PATH "" CACHE PATH
  "Optional LOCPATH for generated glibc stdio locales")
if(ELFCONV_GLIBC_STDIO_LOCALE_PATH)
  foreach(test_name IN LISTS glibc_stdio_registered_tests)
    set_tests_properties("${test_name}" PROPERTIES
      ENVIRONMENT "LOCPATH=${ELFCONV_GLIBC_STDIO_LOCALE_PATH}")
  endforeach()
endif()
