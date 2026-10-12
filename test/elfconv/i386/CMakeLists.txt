option(ELFCONV_I386_TESTS "Run elfconv native-versus-Wasm i386 regressions" OFF)
if(NOT ELFCONV_I386_TESTS)
  return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(ELFCONV_I386_CC NAMES gcc REQUIRED)
find_program(ELFCONV_I386_NODE NAMES node REQUIRED)
find_program(ELFCONV_I386_OBJCOPY NAMES objcopy REQUIRED)
if(NOT ELFCONV_EMCC)
  find_program(ELFCONV_EMCC NAMES em++ REQUIRED)
endif()
add_subdirectory("${CMAKE_SOURCE_DIR}/test/vendored"
  "${CMAKE_CURRENT_BINARY_DIR}/vendored")

foreach(profile IN ITEMS dead-eflags shift-defined double-shift-defined rotate-defined bit-count-defined x87-sm64-defined fxsave-defined fadd-memory-defined x87-environment-defined rdrand-defined rsqrt-defined crypto-defined packed-sse-defined segment-push-defined cpu-defined weak-imports init-array-sentinel pie shared dynamic dynamic-startup dynamic-single-chunk dynamic-chunked rtti libc-adapters math-adapters dirent-adapters time-adapters stat-adapters parse-adapters wide-adapters environment-adapters tls tls-startup tls-gnu2 tls-initial-exec tls-local-dynamic)
  if(profile MATCHES "^dynamic")
    set(fixture_args --source "${CMAKE_CURRENT_SOURCE_DIR}/dynamic-main.c" --dynamic)
    if(profile STREQUAL "dynamic-startup")
      list(APPEND fixture_args --startup-linked)
    elseif(profile STREQUAL "dynamic-single-chunk")
      list(APPEND fixture_args --max-ir-chunks-per-elf 1 --jobs 2)
    elseif(profile STREQUAL "dynamic-chunked")
      list(APPEND fixture_args --max-ir-chunks-per-elf 8 --jobs 2)
    endif()
  elseif(profile STREQUAL "rtti")
    set(fixture_args --source "${CMAKE_CURRENT_SOURCE_DIR}/rtti-main.cpp" --rtti)
  elseif(profile MATCHES "-adapters$")
    set(fixture_args --source "${CMAKE_CURRENT_SOURCE_DIR}/${profile}.c" --no-builtin)
  elseif(profile MATCHES "^tls")
    set(fixture_args --source "${CMAKE_CURRENT_SOURCE_DIR}/tls-main.c" --dynamic --tls)
    if(profile STREQUAL "tls-startup")
      list(APPEND fixture_args --startup-linked)
    elseif(profile STREQUAL "tls-gnu2")
      list(APPEND fixture_args --tls-dialect gnu2)
    elseif(profile STREQUAL "tls-initial-exec")
      list(APPEND fixture_args --tls-model initial-exec --startup-linked)
    elseif(profile STREQUAL "tls-local-dynamic")
      list(APPEND fixture_args --tls-model local-dynamic)
    endif()
  else()
    set(fixture_args --source "${CMAKE_CURRENT_SOURCE_DIR}/${profile}.c")
  endif()
  if(profile STREQUAL "pie")
    list(APPEND fixture_args --pie)
  elseif(profile STREQUAL "shared" OR profile STREQUAL "weak-imports")
    list(APPEND fixture_args --shared)
  endif()
  string(REPLACE "-" "_" test_suffix "${profile}")
  add_test(NAME i386_${test_suffix}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../run_i386.py"
      --lifter "$<TARGET_FILE:elflift>"
      --emcc "${ELFCONV_EMCC}"
      --cc "${ELFCONV_I386_CC}"
      --objcopy "${ELFCONV_I386_OBJCOPY}"
      --node "${ELFCONV_I386_NODE}"
      ${fixture_args}
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/${profile}")
  set_tests_properties(i386_${test_suffix} PROPERTIES
    LABELS "i386;regression" TIMEOUT 1000 RUN_SERIAL TRUE)
endforeach()
add_test(NAME i386_indirect_dispatch
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../run_i386.py"
    --lifter "$<TARGET_FILE:elflift>"
    --emcc "${ELFCONV_EMCC}"
    --cc "${ELFCONV_I386_CC}"
    --objcopy "${ELFCONV_I386_OBJCOPY}"
    --node "${ELFCONV_I386_NODE}"
    --source "${CMAKE_CURRENT_SOURCE_DIR}/indirect-dispatch.c"
    --extra-source "${CMAKE_CURRENT_SOURCE_DIR}/indirect-dispatch.S"
    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/indirect-dispatch")
set_tests_properties(i386_indirect_dispatch PROPERTIES
  LABELS "i386;regression" TIMEOUT 1000 RUN_SERIAL TRUE)

option(ELFCONV_STEAM_TESTS "Run native-versus-Wasm Steam API regression" OFF)
if(ELFCONV_STEAM_TESTS)
  find_program(ELFCONV_I386_CXX NAMES g++ REQUIRED)
  find_program(ELFCONV_I386_READELF NAMES readelf REQUIRED)
  set(ELFCONV_STEAM_API
    "${CMAKE_SOURCE_DIR}/build/gbe_fork/build/linux/gmake/release/regular/x86/libsteam_api.so"
    CACHE FILEPATH "Pinned gbe_fork i386 guest Steam API library")
  add_test(NAME i386_steam
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../run_steam.py"
      --lifter "$<TARGET_FILE:elflift>"
      --emcc "${ELFCONV_EMCC}"
      --node "${ELFCONV_I386_NODE}"
      --cxx "${ELFCONV_I386_CXX}"
      --readelf "${ELFCONV_I386_READELF}"
      --steam-api "${ELFCONV_STEAM_API}"
      --gbe-source "${CMAKE_SOURCE_DIR}/build/gbe_fork"
      --output-dir "${CMAKE_CURRENT_BINARY_DIR}/steam")
  set_tests_properties(i386_steam PROPERTIES
    LABELS "i386;regression;steam" TIMEOUT 1200 RUN_SERIAL TRUE)
endif()
