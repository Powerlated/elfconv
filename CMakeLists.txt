cmake_minimum_required(VERSION 3.21)
project(elfconv LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ELFCONV_ARCH "" CACHE STRING "Lifter architecture: x86 or aarch64")
set_property(CACHE ELFCONV_ARCH PROPERTY STRINGS x86 aarch64)
if(ELFCONV_ARCH STREQUAL "x86")
  set(CMAKE_ELFCONV_X86_BUILD ON)
  set(CMAKE_ELFCONV_AARCH64_BUILD OFF)
elseif(ELFCONV_ARCH STREQUAL "aarch64")
  set(CMAKE_ELFCONV_X86_BUILD OFF)
  set(CMAKE_ELFCONV_AARCH64_BUILD ON)
else()
  message(FATAL_ERROR "Set ELFCONV_ARCH to x86 or aarch64")
endif()

option(ELFCONV_STATIC_ELFLIFT "Statically link elflift" OFF)
set(CMAKE_ELFLIFT_STATIC_LINK "${ELFCONV_STATIC_ELFLIFT}")
set(ELFCONV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(backend/remill)
include(cmake/RemillState.cmake)
add_subdirectory(lifter)
add_subdirectory(examples/hello/c EXCLUDE_FROM_ALL)

include(cmake/ElfconvTargets.cmake)
include(cmake/Sm64exTargets.cmake)
set(ELFCONV_INPUT "" CACHE FILEPATH "Optional ELF to convert with the convert-elf target")
set(ELFCONV_CONVERT_TARGET "" CACHE STRING "Conversion target for ELFCONV_INPUT")
set(ELFCONV_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH "Output directory for convert-elf")
set(ELFCONV_MOUNT_SETTINGS "" CACHE STRING "Semicolon-separated host_dir@/mount/path values")
set(ELFCONV_LEGACY_GL "1" CACHE STRING "Legacy OpenGL emulation for i386 browser builds")
set(ELFCONV_DEBUG_FUNC_ADDR "0" CACHE STRING "Function address passed to the lifter")
set(ELFCONV_BITCODE_PATH "" CACHE FILEPATH "Optional linked semantics bitcode")
set(ELFCONV_WASI_SDK "$ENV{WASI_SDK_PATH}" CACHE PATH "WASI SDK root")
set(ELFCONV_WASMEDGE "" CACHE FILEPATH "WasmEdge executable")
set(ELFCONV_NATIVE_CXX "" CACHE FILEPATH "Native LLVM C++ compiler")
set(ELFCONV_LLVM_DIS "" CACHE FILEPATH "LLVM bitcode disassembler")
if(NOT ELFCONV_NATIVE_CXX)
  if(DEFINED ENV{LLVM_ROOT} AND EXISTS "$ENV{LLVM_ROOT}/bin/clang++")
    set(ELFCONV_NATIVE_CXX "$ENV{LLVM_ROOT}/bin/clang++" CACHE FILEPATH "Native LLVM C++ compiler" FORCE)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(ELFCONV_NATIVE_CXX "${CMAKE_CXX_COMPILER}" CACHE FILEPATH "Native LLVM C++ compiler" FORCE)
  endif()
endif()
if(NOT ELFCONV_LLVM_DIS)
  if(DEFINED ENV{LLVM_ROOT} AND EXISTS "$ENV{LLVM_ROOT}/bin/llvm-dis")
    set(ELFCONV_LLVM_DIS "$ENV{LLVM_ROOT}/bin/llvm-dis" CACHE FILEPATH "LLVM bitcode disassembler" FORCE)
  elseif(CMAKE_CXX_COMPILER)
    get_filename_component(llvm_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${llvm_compiler_dir}/llvm-dis")
      set(ELFCONV_LLVM_DIS "${llvm_compiler_dir}/llvm-dis" CACHE FILEPATH "LLVM bitcode disassembler" FORCE)
    endif()
  endif()
endif()
option(ELFCONV_INIT_WASM "Generate the browser kernel and initial HTML page" OFF)
option(ELFCONV_DEBUG "Enable runtime debug diagnostics" OFF)
option(ELFCONV_FLOAT_STATUS "Enable floating-point exception tracking" OFF)
option(ELFCONV_TEXT_IR "Emit LLVM IR text instead of bitcode" OFF)
option(ELFCONV_NO_LIFTED "Reuse an existing .bc or .ll file" OFF)
option(ELFCONV_NO_COMPILED "Reuse an existing target object file" OFF)

if(ELFCONV_INPUT)
  if(NOT ELFCONV_CONVERT_TARGET)
    message(FATAL_ERROR "Set ELFCONV_CONVERT_TARGET when ELFCONV_INPUT is set")
  endif()
  set(conversion_options)
  foreach(flag IN ITEMS INIT_WASM DEBUG FLOAT_STATUS TEXT_IR NO_LIFTED NO_COMPILED)
    if(ELFCONV_${flag})
      list(APPEND conversion_options "${flag}")
    endif()
  endforeach()
  elfconv_add_conversion_target(convert-elf
    INPUT "${ELFCONV_INPUT}"
    TARGET "${ELFCONV_CONVERT_TARGET}"
    OUTPUT_DIR "${ELFCONV_OUTPUT_DIR}"
    LEGACY_GL "${ELFCONV_LEGACY_GL}"
    DEBUG_FUNC_ADDR "${ELFCONV_DEBUG_FUNC_ADDR}"
    BITCODE_PATH "${ELFCONV_BITCODE_PATH}"
    WASI_SDK "${ELFCONV_WASI_SDK}"
    WASMEDGE "${ELFCONV_WASMEDGE}"
    NATIVE_CXX "${ELFCONV_NATIVE_CXX}"
    LLVM_DIS "${ELFCONV_LLVM_DIS}"
    MOUNT_SETTINGS ${ELFCONV_MOUNT_SETTINGS}
    ${conversion_options}
    DEPENDS elflift
  )
endif()

if(BUILD_TESTING AND CMAKE_ELFCONV_AARCH64_BUILD)
  add_subdirectory(tests/elfconv)
endif()
if(BUILD_TESTING)
  add_subdirectory(tests/browser)
endif()