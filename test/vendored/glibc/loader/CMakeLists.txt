find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME i386_glibc_setjmp
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/test/elfconv/run_i386.py"
    --lifter "$<TARGET_FILE:elflift>"
    --emcc "${ELFCONV_EMCC}"
    --cc "${ELFCONV_I386_CC}"
    --objcopy "${ELFCONV_I386_OBJCOPY}"
    --node "${ELFCONV_I386_NODE}"
    --source "${CMAKE_CURRENT_SOURCE_DIR}/tst-setjmp.c"
    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/setjmp")
set_tests_properties(i386_glibc_setjmp PROPERTIES
  LABELS "i386;regression;glibc;loader" TIMEOUT 1000 RUN_SERIAL TRUE)
