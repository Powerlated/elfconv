# Include from test/elfconv/i386/CMakeLists.txt after its tool discovery.
add_test(NAME i386_glibc_preadvwritev
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/test/elfconv/run_i386.py"
    --lifter "$<TARGET_FILE:elflift>"
    --emcc "${ELFCONV_EMCC}"
    --cc "${ELFCONV_I386_CC}"
    --objcopy "${ELFCONV_I386_OBJCOPY}"
    --node "${ELFCONV_I386_NODE}"
    --source "${CMAKE_CURRENT_SOURCE_DIR}/preadvwritev.c"
    --no-builtin
    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/preadvwritev")
set_tests_properties(i386_glibc_preadvwritev PROPERTIES
  LABELS "i386;regression;filesystem" TIMEOUT 1000 RUN_SERIAL TRUE)
