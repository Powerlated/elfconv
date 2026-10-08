include_guard(GLOBAL)

set(ELFCONV_SM64EX_SOURCE "${ELFCONV_ROOT}/examples/sm64ex" CACHE PATH "SM64EX source directory")
set(ELFCONV_SM64EX_SYSROOT "$ENV{SYSROOT}" CACHE PATH "i386 Linux sysroot for SM64EX")
if(NOT ELFCONV_SM64EX_SYSROOT)
  set(ELFCONV_SM64EX_SYSROOT "$ENV{HOME}/.local/opt/elfconv-i386-sysroot" CACHE PATH "i386 Linux sysroot for SM64EX" FORCE)
endif()
set(ELFCONV_SM64EX_CLANG "$ENV{CLANG}" CACHE FILEPATH "Clang compiler for the i386 SM64EX build")
if(NOT ELFCONV_SM64EX_CLANG)
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(ELFCONV_SM64EX_CLANG "${CMAKE_C_COMPILER}" CACHE FILEPATH "Clang compiler for the i386 SM64EX build" FORCE)
  else()
    find_program(sm64ex_clang NAMES clang HINTS "$ENV{LLVM_ROOT}/bin" REQUIRED)
    set(ELFCONV_SM64EX_CLANG "${sm64ex_clang}" CACHE FILEPATH "Clang compiler for the i386 SM64EX build" FORCE)
  endif()
endif()
set(ELFCONV_SM64EX_DWARF_LIBRARY_DIR "$ENV{HOME}/.local/opt/dwarf-2021/usr/lib/x86_64-linux-gnu" CACHE PATH "Host libdwarf directory used by SM64EX tools")
set(ELFCONV_SM64EX_JOBS 8 CACHE STRING "Parallel jobs for the SM64EX upstream Makefile")
set(ELFCONV_SM64EX_WASM_OPT_LEVEL 1 CACHE STRING "Wasm optimization level for SM64EX (1 for development, 3 for release; 0 can exceed browser Wasm limits)")
option(ELFCONV_SM64EX_JSPI "Use native Wasm stack switching for SM64EX (requires a JSPI-capable browser)" ON)
set(sm64ex_lifter "${ELFCONV_LIFTER}")
if(TARGET elflift)
  set(sm64ex_lifter "$<TARGET_FILE:elflift>")
endif()

foreach(mode IN ITEMS native bitcode wasm)
  add_custom_target(sm64ex-${mode}
    COMMAND "${CMAKE_COMMAND}"
      "-DELFCONV_SM64EX_MODE=${mode}"
      "-DELFCONV_SM64EX_SOURCE=${ELFCONV_SM64EX_SOURCE}"
      "-DELFCONV_SM64EX_SYSROOT=${ELFCONV_SM64EX_SYSROOT}"
      "-DELFCONV_SM64EX_CLANG=${ELFCONV_SM64EX_CLANG}"
      "-DELFCONV_SM64EX_DWARF_LIBRARY_DIR=${ELFCONV_SM64EX_DWARF_LIBRARY_DIR}"
      "-DELFCONV_SM64EX_JOBS=${ELFCONV_SM64EX_JOBS}"
      "-DELFCONV_SM64EX_WASM_OPT_LEVEL=${ELFCONV_SM64EX_WASM_OPT_LEVEL}"
      "-DELFCONV_SM64EX_JSPI=${ELFCONV_SM64EX_JSPI}"
      "-DELFCONV_LIFTER=${sm64ex_lifter}"
      "-DELFCONV_EMCC=${ELFCONV_EMCC}"
      "-DELFCONV_ROOT=${ELFCONV_ROOT}"
      -P "${ELFCONV_ROOT}/cmake/BuildSm64ex.cmake"
    USES_TERMINAL
    VERBATIM
  )
  if(TARGET elflift AND mode MATCHES "^(bitcode|wasm)$")
    add_dependencies(sm64ex-${mode} elflift)
  endif()
endforeach()
