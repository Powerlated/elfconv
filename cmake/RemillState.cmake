include_guard(GLOBAL)

function(_elfconv_add_state_generator target source output)
  add_executable("${target}" EXCLUDE_FROM_ALL "${source}")
  target_include_directories("${target}" PRIVATE "${ELFCONV_ROOT}/backend/remill/include")
  set_target_properties("${target}" PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON)
  target_compile_options("${target}" PRIVATE
    -Wno-nested-anon-types
    -Wno-variadic-macros
    -Wno-invalid-offsetof
    -Wno-return-type-c-linkage
    -Wno-deprecated-literal-operator
  )

  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}"
      "-DELFCONV_GENERATOR=$<TARGET_FILE:${target}>"
      "-DELFCONV_OUTPUT=${output}"
      -P "${ELFCONV_ROOT}/cmake/RunOutputGenerator.cmake"
    DEPENDS "${target}" "${ELFCONV_ROOT}/cmake/RunOutputGenerator.cmake"
    VERBATIM
  )
endfunction()

set(generated_state_files)
if(CMAKE_ELFCONV_X86_BUILD)
  set(x86_state_dir "${CMAKE_BINARY_DIR}/backend/remill/generated/Arch/X86")
  set(x86_save_state "${x86_state_dir}/SaveState.S")
  _elfconv_add_state_generator(
    elfconv-print-x86-save-state
    "${ELFCONV_ROOT}/backend/remill/tests/X86/PrintSaveState.cpp"
    "${x86_save_state}"
  )
  target_compile_options(elfconv-print-x86-save-state PRIVATE -m64)
  target_compile_definitions(elfconv-print-x86-save-state PRIVATE
    ADDRESS_SIZE_BITS=64 HAS_FEATURE_AVX=1 HAS_FEATURE_AVX512=1
  )
  list(APPEND generated_state_files "${x86_save_state}")
  set(state_test_targets
    lift-amd64-tests lift-amd64_avx-tests
    run-amd64-tests run-amd64_avx-tests
  )
elseif(CMAKE_ELFCONV_AARCH64_BUILD)
  set(aarch64_state_dir "${CMAKE_BINARY_DIR}/backend/remill/generated/Arch/AArch64")
  set(aarch64_save_state "${aarch64_state_dir}/SaveState.S")
  set(aarch64_restore_state "${aarch64_state_dir}/RestoreState.S")
  _elfconv_add_state_generator(
    elfconv-print-aarch64-save-state
    "${ELFCONV_ROOT}/backend/remill/tests/AArch64/PrintSaveState.cpp"
    "${aarch64_save_state}"
  )
  _elfconv_add_state_generator(
    elfconv-print-aarch64-restore-state
    "${ELFCONV_ROOT}/backend/remill/tests/AArch64/PrintRestoreState.cpp"
    "${aarch64_restore_state}"
  )
  list(APPEND generated_state_files "${aarch64_save_state}" "${aarch64_restore_state}")
  set(state_test_targets lift-aarch64-tests run-aarch64-tests)
else()
  message(FATAL_ERROR "CMake Remill state generation requires an ELFCONV architecture")
endif()

add_custom_target(elfconv-remill-state DEPENDS ${generated_state_files})
foreach(test_target IN LISTS state_test_targets)
  if(TARGET "${test_target}")
    target_include_directories("${test_target}" PRIVATE "${CMAKE_BINARY_DIR}/backend/remill")
    add_dependencies("${test_target}" elfconv-remill-state)
  endif()
endforeach()
