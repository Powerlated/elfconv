option(ELFCONV_QEMU_I386_TESTS "Run vendored native-versus-Wasm QEMU i386 conformance tests" OFF)
if(NOT ELFCONV_QEMU_I386_TESTS)
  return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(ELFCONV_QEMU_I386_CC NAMES gcc REQUIRED)
find_program(ELFCONV_QEMU_I386_NODE NAMES node REQUIRED)
if(NOT ELFCONV_EMCC)
  find_program(ELFCONV_EMCC NAMES em++ REQUIRED)
endif()

foreach(profile IN ITEMS core full shift-defined double-shift-defined rotate-defined bit-count-defined x87-sm64-defined pie shared)
  if(profile STREQUAL "core" OR profile STREQUAL "full")
    set(fixture_args --profile "${profile}")
  else()
    set(fixture_args --source "${CMAKE_CURRENT_SOURCE_DIR}/${profile}.c")
  endif()
  if(profile STREQUAL "pie")
    list(APPEND fixture_args --pie)
  endif()
  if(profile STREQUAL "shared")
    list(APPEND fixture_args --shared)
  endif()
  string(REPLACE "-" "_" test_suffix "${profile}")
  add_test(NAME qemu_i386_${test_suffix}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run.py"
      --lifter "$<TARGET_FILE:elflift>"
      --emcc "${ELFCONV_EMCC}"
      --cc "${ELFCONV_QEMU_I386_CC}"
      --node "${ELFCONV_QEMU_I386_NODE}"
      --cmake "${CMAKE_COMMAND}"
      ${fixture_args}
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${profile}")
  set_tests_properties(qemu_i386_${test_suffix} PROPERTIES
    LABELS "i386;qemu;conformance" TIMEOUT 1000 RUN_SERIAL TRUE)
endforeach()
