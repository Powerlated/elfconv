option(ELFCONV_GLIBC_PTHREAD_BROWSER_TESTS
  "Run vendored glibc pthread tests in a JSPI-capable browser" OFF)
if(NOT ELFCONV_GLIBC_PTHREAD_BROWSER_TESTS)
  return()
endif()

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
if(NOT ELFCONV_I386_NODE)
  find_program(ELFCONV_I386_NODE NAMES node REQUIRED)
endif()

foreach(test IN ITEMS tsd3 tsd4)
  add_test(NAME i386_glibc_pthread_${test}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/run_browser.py"
      --lifter "$<TARGET_FILE:elflift>"
      --emcc "${ELFCONV_EMCC}"
      --cc "${ELFCONV_I386_CC}"
      --node "${ELFCONV_I386_NODE}"
      --browser-dir "${CMAKE_SOURCE_DIR}/test/browser"
      --source "${CMAKE_CURRENT_LIST_DIR}/tst-${test}.c"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${test}")
  set_tests_properties(i386_glibc_pthread_${test} PROPERTIES
    LABELS "i386;regression;pthread;browser;jspi" TIMEOUT 1000 RUN_SERIAL TRUE)
endforeach()
