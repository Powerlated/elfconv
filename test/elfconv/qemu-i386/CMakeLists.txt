option(ELFCONV_QEMU_I386_TESTS "Run vendored native-versus-Wasm QEMU i386 conformance tests" OFF)
if(NOT ELFCONV_QEMU_I386_TESTS)
  return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(ELFCONV_QEMU_I386_CC NAMES gcc REQUIRED)
find_program(ELFCONV_QEMU_I386_NODE NAMES node REQUIRED)
find_program(ELFCONV_QEMU_I386_OBJCOPY NAMES objcopy REQUIRED)
if(NOT ELFCONV_EMCC)
  find_program(ELFCONV_EMCC NAMES em++ REQUIRED)
endif()

foreach(profile IN ITEMS core full)
  set(fixture_args --profile "${profile}")
  string(REPLACE "-" "_" test_suffix "${profile}")
  add_test(NAME qemu_i386_${test_suffix}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../run_i386.py"
      --lifter "$<TARGET_FILE:elflift>"
      --emcc "${ELFCONV_EMCC}"
      --cc "${ELFCONV_QEMU_I386_CC}"
      --objcopy "${ELFCONV_QEMU_I386_OBJCOPY}"
      --node "${ELFCONV_QEMU_I386_NODE}"
      --cmake "${CMAKE_COMMAND}"
      ${fixture_args}
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${profile}")
  set_tests_properties(qemu_i386_${test_suffix} PROPERTIES
    LABELS "i386;qemu;conformance" TIMEOUT 1000 RUN_SERIAL TRUE)
endforeach()
