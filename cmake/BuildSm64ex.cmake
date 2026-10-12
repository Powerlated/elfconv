cmake_minimum_required(VERSION 3.21)

if(NOT ELFCONV_SM64EX_MODE MATCHES "^(native|bitcode|wasm|emscripten)$")
  message(FATAL_ERROR "SM64EX mode must be native, bitcode, wasm, or emscripten")
endif()
if(NOT IS_DIRECTORY "${ELFCONV_SM64EX_SOURCE}")
  message(FATAL_ERROR "SM64EX source directory not found: ${ELFCONV_SM64EX_SOURCE}; initialize the examples/sm64ex submodule")
endif()
set(game "${ELFCONV_SM64EX_SOURCE}")
if(NOT EXISTS "${game}/baserom.us.z64")
  message(FATAL_ERROR "Missing ${game}/baserom.us.z64; provide the ROM locally and do not commit it")
endif()
if(ELFCONV_SM64EX_MODE STREQUAL "emscripten")
  if(ELFCONV_SM64EX_SHARED)
    message(FATAL_ERROR "SM64EX Emscripten builds do not support ELFCONV_SM64EX_SHARED")
  endif()
  if(NOT EXISTS "${ELFCONV_EMCC_C}")
    message(FATAL_ERROR "Emscripten C compiler not found: ${ELFCONV_EMCC_C}")
  endif()
else()
  if(NOT IS_DIRECTORY "${ELFCONV_SM64EX_SYSROOT}")
    message(FATAL_ERROR "i386 sysroot not found: ${ELFCONV_SM64EX_SYSROOT}")
  endif()
  if(NOT EXISTS "${ELFCONV_SM64EX_CLANG}")
    message(FATAL_ERROR "SM64EX Clang compiler not found: ${ELFCONV_SM64EX_CLANG}")
  endif()
endif()

find_program(make_program NAMES gmake make REQUIRED)
find_program(host_c_compiler NAMES gcc cc REQUIRED)
find_program(host_cxx_compiler NAMES g++ c++ REQUIRED)
if(NOT ELFCONV_SM64EX_JOBS)
  set(ELFCONV_SM64EX_JOBS 8)
endif()

execute_process(
  COMMAND "${make_program}" -C "${game}/tools/audiofile" "CC=${host_c_compiler}" "CXX=${host_cxx_compiler}"
  RESULT_VARIABLE result
  COMMAND_ECHO STDOUT
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Building SM64EX audio tools failed with exit code ${result}")
endif()
execute_process(
  COMMAND "${make_program}" -C "${game}/tools" "CC=${host_c_compiler}" "CXX=${host_cxx_compiler}"
  RESULT_VARIABLE result
  COMMAND_ECHO STDOUT
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Building SM64EX asset tools failed with exit code ${result}")
endif()

if(ELFCONV_SM64EX_MODE STREQUAL "emscripten")
  set(build_dir "build/us_web_emscripten")
  set(elf "${game}/${build_dir}/sm64.us.f3dex2e.html")
  set(link_flags)
elseif(ELFCONV_SM64EX_SHARED)
  set(build_dir "build/us_pc_shared")
  set(elf "${game}/${build_dir}/sm64.us.f3dex2e.so")
  set(pic_flag -fPIC)
  set(link_flags "-shared -Wl,-z,defs")
else()
  set(build_dir "build/us_pc_pie")
  set(elf "${game}/${build_dir}/sm64.us.f3dex2e")
  set(pic_flag -fPIE)
  set(link_flags -pie)
endif()
set(link_map)
if(ELFCONV_SM64EX_MODE STREQUAL "wasm")
  set(link_map "${elf}.map")
  if(NOT EXISTS "${link_map}")
    file(REMOVE "${elf}")
  endif()
endif()
if(ELFCONV_SM64EX_MODE STREQUAL "emscripten")
  set(sm64ex_environment)
  set(sm64ex_make_args
    "${make_program}" -C "${game}" "-j${ELFCONV_SM64EX_JOBS}"
    TARGET_WEB=1 "BUILD_DIR=${build_dir}" "EXE=${elf}"
    "CC=${ELFCONV_EMCC_C}" "CXX=${ELFCONV_EMCC_C}"
    "SDLCONFIG=pkg-config sdl2"
  )
else()
  set(sysroot "${ELFCONV_SM64EX_SYSROOT}")
  set(platform_cflags
    "${pic_flag} --sysroot=${sysroot} -I${sysroot}/usr/include/i386-linux-gnu -B${sysroot}/usr/lib/i386-linux-gnu -Wno-unused-command-line-argument"
  )
  set(pkg_config_libdir "${sysroot}/usr/lib/i386-linux-gnu/pkgconfig:${sysroot}/usr/share/pkgconfig")
  set(ld_library_path "${ELFCONV_SM64EX_DWARF_LIBRARY_DIR}")
  if(DEFINED ENV{LD_LIBRARY_PATH} AND NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
    string(APPEND ld_library_path ":$ENV{LD_LIBRARY_PATH}")
  endif()
  set(sm64ex_environment
    "SYSROOT=${sysroot}"
    "PKG_CONFIG_SYSROOT_DIR=${sysroot}"
    "PKG_CONFIG_LIBDIR=${pkg_config_libdir}"
    "LD_LIBRARY_PATH=${ld_library_path}"
  )
  set(sm64ex_make_args
    "${make_program}" -C "${game}" "-j${ELFCONV_SM64EX_JOBS}"
    TARGET_BITS=32 TARGET_ARCH=i686 NO_PIE=0 "BUILD_DIR=${build_dir}" "EXE=${elf}"
    "CC=${ELFCONV_SM64EX_CLANG}" "PLATFORM_CFLAGS=${platform_cflags}"
    "SDLCONFIG=pkg-config sdl2"
  )
endif()
set(link_map_makefile "${CMAKE_CURRENT_BINARY_DIR}/sm64ex-link-map.mk")
if(ELFCONV_SM64EX_MODE STREQUAL "emscripten")
  file(WRITE "${link_map_makefile}"
    "LDFLAGS = -lm -lGL -lSDL2 -no-pie -s TOTAL_MEMORY=64MB -g4 --source-map-base http://localhost:8080/ -s \"EXPORTED_RUNTIME_METHODS=['callMain']\"\n"
  )
else()
  file(WRITE "${link_map_makefile}" "LDFLAGS += ${link_flags}\n")
endif()
if(link_map)
  file(APPEND "${link_map_makefile}" "LDFLAGS += -Wl,-Map,${link_map}\n")
endif()
list(APPEND sm64ex_make_args -f "${game}/Makefile" -f "${link_map_makefile}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env ${sm64ex_environment} ${sm64ex_make_args}
  RESULT_VARIABLE result
  COMMAND_ECHO STDOUT
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Building the SM64EX ${ELFCONV_SM64EX_MODE} output failed with exit code ${result}")
endif()

if(NOT EXISTS "${elf}")
  message(FATAL_ERROR "SM64EX build did not produce ${elf}")
endif()
if(ELFCONV_SM64EX_MODE MATCHES "^(native|emscripten)$")
  return()
endif()
set(entry_args)
if(ELFCONV_SM64EX_SHARED)
  set(entry_args --entry_symbol main)
  execute_process(
    COMMAND "${ELFCONV_SM64EX_CLANG}" -m32 "--sysroot=${sysroot}"
      "-B${sysroot}/usr/lib/i386-linux-gnu"
      "${ELFCONV_ROOT}/examples/sm64-shared-runner.c" -ldl
      -o "${game}/${build_dir}/sm64-runner"
    RESULT_VARIABLE result
    COMMAND_ECHO STDOUT)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Building SM64EX shared-library runner failed")
  endif()
endif()
if(ELFCONV_SM64EX_MODE STREQUAL "bitcode")
  if(NOT EXISTS "${ELFCONV_LIFTER}")
    message(FATAL_ERROR "ELF lifter not found: ${ELFCONV_LIFTER}")
  endif()
  execute_process(
    COMMAND "${ELFCONV_LIFTER}" --arch i386 --target_arch emscripten32 --target_elf "${elf}"
      --bc_out "${elf}.bc" --norm_mode 1 --fork_emulation 0 --float_exception 0
      ${entry_args}
    RESULT_VARIABLE result
    COMMAND_ECHO STDOUT
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Lifting the SM64EX ELF failed with exit code ${result}")
  endif()
else()
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  set(converter_flags)
  if(ELFCONV_SM64EX_SHARED)
    list(APPEND converter_flags --entry-symbol main)
  endif()
  if(ELFCONV_SM64EX_JSPI)
    list(APPEND converter_flags --jspi)
  endif()
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${ELFCONV_ROOT}/scripts/convert_elf.py"
      --input "${elf}" --target i386-wasm
      --output-dir "${game}/${build_dir}"
      --lifter "${ELFCONV_LIFTER}" --emcc "${ELFCONV_EMCC}"
      --wasm-opt-level "${ELFCONV_SM64EX_WASM_OPT_LEVEL}"
      --legacy-gl 0 --incremental-units
      --link-map "${link_map}" --object-base "${game}"
      ${converter_flags}
    RESULT_VARIABLE result
    COMMAND_ECHO STDOUT
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Converting the SM64EX ELF to Wasm failed with exit code ${result}")
  endif()
endif()
