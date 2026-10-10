include_guard(GLOBAL)

if(NOT DEFINED ELFCONV_ROOT)
  get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(ELFCONV_LIFTER "${ELFCONV_ROOT}/build/src/lifter/elflift" CACHE FILEPATH "ELF conversion lifter")
set(ELFCONV_EMCC "" CACHE FILEPATH "Emscripten C++ compiler")

function(elfconv_add_conversion_target name)
  set(options INIT_WASM DEBUG FLOAT_STATUS TEXT_IR NO_LIFTED NO_COMPILED READY_JS INCREMENTAL_UNITS JSPI PTHREADS)
  set(one_value_args INPUT TARGET OUTPUT_DIR ELF_NAME ENTRY_SYMBOL LIFTER EMCC LEGACY_GL DEBUG_FUNC_ADDR BITCODE_PATH WASI_SDK WASMEDGE NATIVE_CXX LLVM_DIS OUTPUT_EXECUTABLE LINK_MAP OBJECT_BASE WASM_OPT_LEVEL)
  set(multi_value_args DEPENDS MOUNT_SETTINGS SHARED_LIBRARIES)
  cmake_parse_arguments(PARSE_ARGV 1 ECV "${options}" "${one_value_args}" "${multi_value_args}")

  if(ECV_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown arguments for ${name}: ${ECV_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT ECV_INPUT OR NOT ECV_TARGET)
    message(FATAL_ERROR "${name} requires INPUT and TARGET")
  endif()
  if(NOT ECV_OUTPUT_DIR)
    set(ECV_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  set(use_lifter_target OFF)
  if(NOT ECV_LIFTER)
    if(TARGET elflift)
      set(ECV_LIFTER "$<TARGET_FILE:elflift>")
      set(use_lifter_target ON)
    else()
      set(ECV_LIFTER "${ELFCONV_LIFTER}")
    endif()
  endif()
  if(NOT ECV_EMCC)
    set(ECV_EMCC "${ELFCONV_EMCC}")
  endif()

  set(converter_args
    "-DELFCONV_INPUT=${ECV_INPUT}"
    "-DELFCONV_TARGET=${ECV_TARGET}"
    "-DELFCONV_OUTPUT_DIR=${ECV_OUTPUT_DIR}"
    "-DELFCONV_LIFTER=${ECV_LIFTER}"
  )
  foreach(variable IN ITEMS ELF_NAME ENTRY_SYMBOL EMCC LEGACY_GL DEBUG_FUNC_ADDR BITCODE_PATH WASI_SDK WASMEDGE NATIVE_CXX LLVM_DIS OUTPUT_EXECUTABLE LINK_MAP OBJECT_BASE WASM_OPT_LEVEL)
    if(DEFINED ECV_${variable} AND NOT ECV_${variable} STREQUAL "")
      list(APPEND converter_args "-DELFCONV_${variable}=${ECV_${variable}}")
    endif()
  endforeach()
  foreach(flag IN ITEMS INIT_WASM DEBUG FLOAT_STATUS TEXT_IR NO_LIFTED NO_COMPILED READY_JS INCREMENTAL_UNITS PTHREADS)
    if(ECV_${flag})
      list(APPEND converter_args "-DELFCONV_${flag}=ON")
    endif()
  endforeach()
  if(ECV_JSPI)
    list(APPEND converter_args "-DELFCONV_WASM_JSPI=ON")
  endif()
  if(ECV_MOUNT_SETTINGS)
    string(REPLACE ";" "\\;" escaped_mount_settings "${ECV_MOUNT_SETTINGS}")
    list(APPEND converter_args "-DELFCONV_MOUNT_SETTINGS=${escaped_mount_settings}")
  endif()
  if(ECV_SHARED_LIBRARIES)
    string(REPLACE ";" "\\;" escaped_libraries "${ECV_SHARED_LIBRARIES}")
    list(APPEND converter_args "-DELFCONV_SHARED_LIBRARIES=${escaped_libraries}")
  endif()

  set(file_dependencies)
  set(target_dependencies)
  foreach(dependency IN LISTS ECV_DEPENDS)
    if(TARGET "${dependency}")
      list(APPEND target_dependencies "${dependency}")
    else()
      list(APPEND file_dependencies "${dependency}")
    endif()
  endforeach()

  add_custom_target("${name}"
    COMMAND "${CMAKE_COMMAND}" ${converter_args} -P "${ELFCONV_ROOT}/cmake/ConvertElf.cmake"
    DEPENDS ${file_dependencies}
    USES_TERMINAL
    VERBATIM
  )
  if(target_dependencies)
    add_dependencies("${name}" ${target_dependencies})
  endif()
  if(use_lifter_target)
    add_dependencies("${name}" elflift)
  endif()
endfunction()
