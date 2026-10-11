option(ELFCONV_GLIBC_PTHREAD_BROWSER_TESTS
  "Run vendored glibc pthread tests in a JSPI-capable browser" OFF)
if(NOT ELFCONV_GLIBC_PTHREAD_BROWSER_TESTS)
  return()
endif()

if(NOT ELFCONV_I386_PTHREAD_BROWSER_RUNNER)
  message(FATAL_ERROR
    "ELFCONV_GLIBC_PTHREAD_BROWSER_TESTS requires ELFCONV_I386_PTHREAD_BROWSER_RUNNER; "
    "run_i386.py's Node mode does not execute real Emscripten threads")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

foreach(test IN ITEMS tsd3 tsd4)
  add_test(NAME i386_glibc_pthread_${test}
    COMMAND "${Python3_EXECUTABLE}" "${ELFCONV_I386_PTHREAD_BROWSER_RUNNER}"
      --lifter "$<TARGET_FILE:elflift>"
      --emcc "${ELFCONV_EMCC}"
      --cc "${ELFCONV_I386_CC}"
      --source "${CMAKE_CURRENT_LIST_DIR}/tst-${test}.c"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${test}")
  set_tests_properties(i386_glibc_pthread_${test} PROPERTIES
    LABELS "i386;regression;pthread;browser;jspi" TIMEOUT 1000 RUN_SERIAL TRUE)
endforeach()
