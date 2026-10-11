set(ELFCONV_GLIBC_LOCALE_PATH "" CACHE PATH
  "Optional LOCPATH for generated glibc locales")
foreach(profile IN ITEMS glibc_mbrtowc)
  string(REPLACE "glibc_" "" source_name "${profile}")
  add_test(NAME i386_${profile}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../elfconv/run_i386.py"
      --lifter "$<TARGET_FILE:elflift>"
      --emcc "${ELFCONV_EMCC}"
      --cc "${ELFCONV_I386_CC}"
      --objcopy "${ELFCONV_I386_OBJCOPY}"
      --node "${ELFCONV_I386_NODE}"
      --source "${CMAKE_CURRENT_SOURCE_DIR}/tst-${source_name}.c"
      --no-builtin
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${profile}")
  set_tests_properties(i386_${profile} PROPERTIES
    LABELS "i386;regression;glibc" TIMEOUT 1000 RUN_SERIAL TRUE)
  if(ELFCONV_GLIBC_LOCALE_PATH)
    set_tests_properties(i386_${profile} PROPERTIES
      ENVIRONMENT "LOCPATH=${ELFCONV_GLIBC_LOCALE_PATH}")
  endif()
endforeach()
