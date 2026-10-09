cmake_minimum_required(VERSION 3.21)

get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(IS_DIRECTORY "${ELFCONV_ROOT}/lib")
  if(DEFINED ENV{LD_LIBRARY_PATH} AND NOT "$ENV{LD_LIBRARY_PATH}" STREQUAL "")
    set(ENV{LD_LIBRARY_PATH} "${ELFCONV_ROOT}/lib:$ENV{LD_LIBRARY_PATH}")
  else()
    set(ENV{LD_LIBRARY_PATH} "${ELFCONV_ROOT}/lib")
  endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/PackPreload.cmake")

if(NOT DEFINED ELFCONV_INPUT OR ELFCONV_INPUT STREQUAL "")
  message(FATAL_ERROR "Set ELFCONV_INPUT to the ELF file to convert")
endif()
if(NOT EXISTS "${ELFCONV_INPUT}")
  message(FATAL_ERROR "ELF input does not exist: ${ELFCONV_INPUT}")
endif()
if(NOT DEFINED ELFCONV_TARGET OR ELFCONV_TARGET STREQUAL "")
  message(FATAL_ERROR "Set ELFCONV_TARGET (for example i386-wasm or aarch64-wasm)")
endif()
if(NOT ELFCONV_TARGET MATCHES "^(aarch64|amd64|i386)-(native|wasm|wasi32)$")
  message(FATAL_ERROR "Unsupported ELFCONV_TARGET: ${ELFCONV_TARGET}")
endif()

file(REAL_PATH "${ELFCONV_INPUT}" ELFCONV_INPUT)
if(DEFINED ELFCONV_ELF_NAME AND NOT ELFCONV_ELF_NAME STREQUAL "")
  set(ELFCONV_NAME "${ELFCONV_ELF_NAME}")
else()
  cmake_path(GET ELFCONV_INPUT FILENAME ELFCONV_NAME)
endif()
if(NOT DEFINED ELFCONV_OUTPUT_DIR OR ELFCONV_OUTPUT_DIR STREQUAL "")
  set(ELFCONV_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()
file(MAKE_DIRECTORY "${ELFCONV_OUTPUT_DIR}")
file(REAL_PATH "${ELFCONV_OUTPUT_DIR}" ELFCONV_OUTPUT_DIR)

set(ELFCONV_RUNTIME_DIR "${ELFCONV_ROOT}/runtime")
set(ELFCONV_UTILS_DIR "${ELFCONV_ROOT}/utils")
set(ELFCONV_BROWSER_DIR "${ELFCONV_ROOT}/browser")
set(ELFCONV_COMMON_RUNTIME_SOURCES
  "${ELFCONV_RUNTIME_DIR}/Entry.cpp"
  "${ELFCONV_RUNTIME_DIR}/Memory.cpp"
  "${ELFCONV_RUNTIME_DIR}/Runtime.cpp"
  "${ELFCONV_RUNTIME_DIR}/VmIntrinsics.cpp"
  "${ELFCONV_UTILS_DIR}/Util.cpp"
  "${ELFCONV_UTILS_DIR}/elfconv.cpp"
)
set(ELFCONV_RUNTIME_INCLUDE_FLAGS
  "-I${ELFCONV_ROOT}/backend/remill/include"
  "-I${ELFCONV_ROOT}"
)
set(ELFCONV_RUNTIME_DEFINITIONS)
if(ELFCONV_TARGET MATCHES "^aarch64-")
  list(APPEND ELFCONV_RUNTIME_DEFINITIONS -DELF_IS_AARCH64)
elseif(ELFCONV_TARGET MATCHES "^amd64-")
  list(APPEND ELFCONV_RUNTIME_DEFINITIONS -DELF_IS_AMD64)
endif()
if(ELFCONV_DEBUG)
  list(APPEND ELFCONV_RUNTIME_DEFINITIONS
    -DELFC_RUNTIME_SYSCALL_DEBUG=1
    -DELFC_RUNTIME_MULSECTIONS_WARNING=1
  )
endif()
if(NOT DEFINED ELFCONV_DEBUG_FUNC_ADDR OR ELFCONV_DEBUG_FUNC_ADDR STREQUAL "")
  set(ELFCONV_DEBUG_FUNC_ADDR 0)
endif()
if(ELFCONV_FLOAT_STATUS)
  set(ELFCONV_FLOAT_EXCEPTION 1)
else()
  set(ELFCONV_FLOAT_EXCEPTION 0)
endif()

function(_elfconv_execute description)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE result
    COMMAND_ECHO STDOUT
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${description} failed with exit code ${result}")
  endif()
endfunction()

function(_elfconv_prepare_browser)
  set(generated_js "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.generated.js")
  set(generated_wasm "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.generated.wasm")
  set(output_js "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.js")
  set(output_wasm "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.wasm")
  set(output_html "${ELFCONV_OUTPUT_DIR}/main.html")
  if(NOT EXISTS "${generated_js}" OR NOT EXISTS "${generated_wasm}")
    message(FATAL_ERROR "Expected generated browser outputs ${generated_js} and ${generated_wasm}")
  endif()

  file(COPY_FILE "${generated_wasm}" "${output_wasm}")
  file(COPY_FILE "${ELFCONV_BROWSER_DIR}/coi-serviceworker.js" "${ELFCONV_OUTPUT_DIR}/coi-serviceworker.js")

  file(READ "${generated_js}" generated_source)
  foreach(symbol IN ITEMS me_forked me_execved)
    string(REGEX MATCH "Module\\[\"_${symbol}\"\\][ \t]*=[ \t]*([0-9]+)" symbol_match "${generated_source}")
    if(NOT symbol_match)
      message(FATAL_ERROR "Generated JavaScript does not define Module[\"_${symbol}\"]")
    endif()
    set(${symbol}_address "${CMAKE_MATCH_1}")
  endforeach()

  file(READ "${ELFCONV_BROWSER_DIR}/process.js" process_source)
  string(REGEX REPLACE "(var[ \t]+meForkedP[ \t]*=[ \t]*)[0-9]+;" "\\1${me_forked_address};" process_source "${process_source}")
  string(REGEX REPLACE "(var[ \t]+meExecvedP[ \t]*=[ \t]*)[0-9]+;" "\\1${me_execved_address};" process_source "${process_source}")
  file(WRITE "${output_js}" "${process_source}")

  if(ELFCONV_INIT_WASM)
    file(COPY_FILE "${ELFCONV_BROWSER_DIR}/js-kernel.js" "${ELFCONV_OUTPUT_DIR}/js-kernel.js")
    file(COPY_FILE "${ELFCONV_BROWSER_DIR}/main.html" "${output_html}")
  elseif(NOT EXISTS "${output_html}")
    message(FATAL_ERROR "${output_html} is missing; provide an init page or set ELFCONV_INIT_WASM")
  endif()

  file(READ "${output_html}" html_source)
  string(REGEX REPLACE "initProgram: '[^']*\\.wasm'" "initProgram: '${ELFCONV_NAME}.wasm'" html_source "${html_source}")
  string(REGEX MATCH "var binList = \\[[^]]*\\]" bin_list "${html_source}")
  if(NOT bin_list)
    message(FATAL_ERROR "${output_html} does not contain the expected 'var binList = [...]' declaration")
  endif()
  string(REGEX REPLACE "^var binList = \\[([^]]*)\\]$" "\\1" bin_names "${bin_list}")
  _elfconv_json_escape("${ELFCONV_NAME}" escaped_name)
  if(NOT bin_names MATCHES "(^|,)[ \t]*\"${escaped_name}\"([ \t]*,|$)")
    if(bin_names STREQUAL "")
      set(updated_bin_list "var binList = [\"${escaped_name}\"]")
    else()
      set(updated_bin_list "var binList = [${bin_names}, \"${escaped_name}\"]")
    endif()
    string(REPLACE "${bin_list}" "${updated_bin_list}" html_source "${html_source}")
  endif()
  file(WRITE "${output_html}" "${html_source}")
endfunction()

if(ELFCONV_READY_JS)
  _elfconv_prepare_browser()
  return()
endif()

if(NOT DEFINED ELFCONV_LIFTER OR ELFCONV_LIFTER STREQUAL "")
  if(EXISTS "${ELFCONV_ROOT}/bin/elflift")
    set(ELFCONV_LIFTER "${ELFCONV_ROOT}/bin/elflift")
  else()
    set(ELFCONV_LIFTER "${ELFCONV_ROOT}/build/lifter/elflift")
  endif()
endif()
if(NOT DEFINED ELFCONV_BITCODE_PATH AND IS_DIRECTORY "${ELFCONV_ROOT}/bitcode")
  set(ELFCONV_BITCODE_PATH "${ELFCONV_ROOT}/bitcode")
endif()

set(elfconv_entry_args)
if(ELFCONV_ENTRY_SYMBOL)
  if(NOT ELFCONV_TARGET MATCHES "^i386-")
    message(FATAL_ERROR "ELFCONV_ENTRY_SYMBOL currently supports i386 only")
  endif()
  list(APPEND elfconv_entry_args --entry_symbol "${ELFCONV_ENTRY_SYMBOL}")
endif()
if(ELFCONV_INCREMENTAL_UNITS)
  if(NOT ELFCONV_TARGET STREQUAL "i386-wasm")
    message(FATAL_ERROR "Incremental unit conversion currently supports i386-wasm only")
  endif()
  include("${ELFCONV_ROOT}/cmake/IncrementalWasm.cmake")
  return()
endif()
if(NOT ELFCONV_NO_LIFTED)
  if(NOT EXISTS "${ELFCONV_LIFTER}")
    message(FATAL_ERROR "ELF lifter not found: ${ELFCONV_LIFTER}")
  endif()
  if(ELFCONV_TARGET MATCHES "^i386-")
    set(elf_arch i386)
    set(target_arch emscripten32)
  elseif(ELFCONV_TARGET MATCHES "^aarch64-")
    set(elf_arch aarch64)
    set(target_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  else()
    set(elf_arch amd64)
    set(target_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  endif()
  if(ELFCONV_TARGET MATCHES "-wasi32$")
    set(target_arch wasi32)
  endif()
  if(ELFCONV_TARGET STREQUAL "i386-wasm")
    set(fork_emulation 0)
  elseif(ELFCONV_TARGET MATCHES "-wasm$")
    set(fork_emulation 1)
  else()
    set(fork_emulation 0)
  endif()

  set(lifter_args
    --arch "${elf_arch}"
    --bc_out "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.bc"
    --target_elf "${ELFCONV_INPUT}"
    --dbg_fun_vma "${ELFCONV_DEBUG_FUNC_ADDR}"
    --target_arch "${target_arch}"
    --float_exception "${ELFCONV_FLOAT_EXCEPTION}"
    --norm_mode 1
    --fork_emulation "${fork_emulation}"
  )
  list(APPEND lifter_args ${elfconv_entry_args})
  if(DEFINED ELFCONV_BITCODE_PATH AND NOT ELFCONV_BITCODE_PATH STREQUAL "")
    list(APPEND lifter_args --bitcode_path "${ELFCONV_BITCODE_PATH}")
  endif()
  _elfconv_execute("ELF lifting" "${ELFCONV_LIFTER}" ${lifter_args})
  set(main_ir "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.bc")
else()
  foreach(extension IN ITEMS ll bc)
    if(EXISTS "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.${extension}")
      set(main_ir "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.${extension}")
      break()
    endif()
  endforeach()
  if(NOT main_ir)
    message(FATAL_ERROR "No lifted .ll or .bc file found for ${ELFCONV_NAME} in ${ELFCONV_OUTPUT_DIR}")
  endif()
endif()

if(ELFCONV_TEXT_IR)
  if(EXISTS "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.bc")
    if(DEFINED ENV{LLVM_ROOT} AND EXISTS "$ENV{LLVM_ROOT}/bin/llvm-dis")
      set(ELFCONV_LLVM_DIS "$ENV{LLVM_ROOT}/bin/llvm-dis")
    else()
      find_program(ELFCONV_LLVM_DIS
        NAMES llvm-dis-23 llvm-dis-22 llvm-dis-21 llvm-dis-20 llvm-dis-19 llvm-dis-18 llvm-dis-17 llvm-dis-16 llvm-dis
        REQUIRED
      )
    endif()
    set(bitcode_path "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.bc")
    set(main_ir "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.ll")
    _elfconv_execute("LLVM bitcode disassembly" "${ELFCONV_LLVM_DIS}" "${bitcode_path}" -o "${main_ir}")
    file(REMOVE "${bitcode_path}")
  elseif(NOT EXISTS "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.ll")
    message(FATAL_ERROR "ELFCONV_TEXT_IR requires an existing .bc or .ll file")
  endif()
endif()

if(ELFCONV_TARGET MATCHES "-native$")
  if(DEFINED ENV{LLVM_ROOT} AND EXISTS "$ENV{LLVM_ROOT}/bin/clang++")
    set(ELFCONV_NATIVE_CXX "$ENV{LLVM_ROOT}/bin/clang++")
  else()
    find_program(ELFCONV_NATIVE_CXX
      NAMES clang++-23 clang++-22 clang++-21 clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++
      REQUIRED
    )
  endif()
  set(runtime_definitions ${ELFCONV_RUNTIME_DEFINITIONS} "-DELFNAME=\"${ELFCONV_NAME}\"")
  set(common_flags -O3 -std=c++20 -static ${ELFCONV_RUNTIME_INCLUDE_FLAGS} ${runtime_definitions})
  set(main_object "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.o")
  if(NOT ELFCONV_NO_COMPILED)
    _elfconv_execute("Native bitcode compilation" "${ELFCONV_NATIVE_CXX}" ${common_flags} -c "${main_ir}" -o "${main_object}")
  endif()
  set(output_binary "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.${CMAKE_HOST_SYSTEM_PROCESSOR}")
  _elfconv_execute("Native executable link" "${ELFCONV_NATIVE_CXX}" ${common_flags} -o "${output_binary}" "${main_object}" ${ELFCONV_COMMON_RUNTIME_SOURCES} "${ELFCONV_RUNTIME_DIR}/syscalls/SyscallNative.cpp")
  if(ELFCONV_OUTPUT_EXECUTABLE)
    file(RENAME "${output_binary}" "${ELFCONV_OUTPUT_EXECUTABLE}")
  endif()
  return()
endif()

if(ELFCONV_TARGET STREQUAL "i386-wasm")
  if(NOT DEFINED ELFCONV_EMCC OR ELFCONV_EMCC STREQUAL "")
    find_program(ELFCONV_EMCC NAMES em++ HINTS "$ENV{EMSDK}/upstream/emscripten" REQUIRED)
  endif()
  if(NOT DEFINED ELFCONV_LEGACY_GL OR ELFCONV_LEGACY_GL STREQUAL "")
    set(ELFCONV_LEGACY_GL 1)
  endif()
  if(NOT DEFINED ELFCONV_WASM_OPT_LEVEL OR ELFCONV_WASM_OPT_LEVEL STREQUAL "")
    set(ELFCONV_WASM_OPT_LEVEL 3)
  endif()
  if(NOT ELFCONV_WASM_OPT_LEVEL MATCHES "^[0123sz]$")
    message(FATAL_ERROR "ELFCONV_WASM_OPT_LEVEL must be 0, 1, 2, 3, s, or z")
  endif()
  set(main_object "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.wasm.o")
  if(NOT ELFCONV_NO_COMPILED AND
     (NOT EXISTS "${main_object}" OR "${main_ir}" IS_NEWER_THAN "${main_object}" OR
      "${ELFCONV_EMCC}" IS_NEWER_THAN "${main_object}"))
    _elfconv_execute("i386 bitcode compilation"
      "${ELFCONV_EMCC}" "-O${ELFCONV_WASM_OPT_LEVEL}" -c "${main_ir}" -o "${main_object}")
  endif()
  if(NOT EXISTS "${main_object}")
    message(FATAL_ERROR "Missing compiled Wasm object: ${main_object}")
  endif()
  if(ELFCONV_WASM_JSPI)
    set(ELFCONV_BROWSER_JSPI true)
    set(suspension_flags -sJSPI=1)
  else()
    set(ELFCONV_BROWSER_JSPI false)
    set(suspension_flags -sASYNCIFY=1)
  endif()
  set(debug_flags)
  if(ELFCONV_WASM_OPT_LEVEL STREQUAL "0")
    set(debug_flags -g2)
  endif()
  _elfconv_execute("i386 browser link"
    "${ELFCONV_EMCC}" "-O${ELFCONV_WASM_OPT_LEVEL}" ${debug_flags} ${ELFCONV_RUNTIME_INCLUDE_FLAGS} -std=c++17
    ${ELFCONV_RUNTIME_DEFINITIONS} -DELF_IS_I386 -DADDRESS_SIZE_BITS=32
    -sUSE_SDL=2 "-sLEGACY_GL_EMULATION=${ELFCONV_LEGACY_GL}"
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    "-DECV_LEGACY_GL=${ELFCONV_LEGACY_GL}" ${suspension_flags}
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=335544320 -sSTACK_SIZE=1048576 -sEXIT_RUNTIME=1
    "${main_object}" ${ELFCONV_COMMON_RUNTIME_SOURCES} "${ELFCONV_RUNTIME_DIR}/I386Imports.cpp"
    -o "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.js"
  )
  configure_file("${ELFCONV_ROOT}/browser/i386.html.in"
    "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.html" @ONLY)
  return()
endif()

if(ELFCONV_TARGET MATCHES "-wasm$")
  if(NOT DEFINED ELFCONV_EMCC OR ELFCONV_EMCC STREQUAL "")
    find_program(ELFCONV_EMCC NAMES em++ HINTS "$ENV{EMSDK}/upstream/emscripten" REQUIRED)
  endif()
  set(runtime_definitions ${ELFCONV_RUNTIME_DEFINITIONS} -DTARGET_IS_BROWSER=1 "-DELFNAME=\"${ELFCONV_NAME}\"")
  set(main_object "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.wasm.o")
  set(generated_js "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.generated.js")
  if(NOT ELFCONV_NO_COMPILED)
    _elfconv_execute("Browser bitcode compilation" "${ELFCONV_EMCC}" -O3 ${ELFCONV_RUNTIME_INCLUDE_FLAGS} ${runtime_definitions} -c "${main_ir}" -o "${main_object}")
  elseif(NOT EXISTS "${main_object}")
    message(FATAL_ERROR "ELFCONV_NO_COMPILED requires ${main_object}")
  endif()
  _elfconv_execute("Browser Wasm link"
    "${ELFCONV_EMCC}" -O3 ${ELFCONV_RUNTIME_INCLUDE_FLAGS} ${runtime_definitions}
    -sASYNCIFY=0 -sINITIAL_MEMORY=536870912 -sSTACK_SIZE=16MB
    -sPTHREAD_POOL_SIZE=0 -pthread -sALLOW_MEMORY_GROWTH -sEXPORT_ES6
    -sENVIRONMENT=web,worker -o "${generated_js}" "${main_object}"
    ${ELFCONV_COMMON_RUNTIME_SOURCES} "${ELFCONV_RUNTIME_DIR}/syscalls/SyscallBrowser.cpp"
  )
  if(ELFCONV_MOUNT_SETTINGS)
    elfconv_pack_preload("${ELFCONV_MOUNT_SETTINGS}" "${ELFCONV_OUTPUT_DIR}")
  endif()
  _elfconv_prepare_browser()
  return()
endif()

if(ELFCONV_TARGET MATCHES "-wasi32$")
  if(NOT DEFINED ELFCONV_WASI_SDK OR ELFCONV_WASI_SDK STREQUAL "")
    set(ELFCONV_WASI_SDK "$ENV{WASI_SDK_PATH}")
  endif()
  if(NOT ELFCONV_WASI_SDK)
    message(FATAL_ERROR "Set ELFCONV_WASI_SDK or WASI_SDK_PATH")
  endif()
  set(wasi_cxx "${ELFCONV_WASI_SDK}/bin/clang++")
  set(wasi_sysroot "${ELFCONV_WASI_SDK}/share/wasi-sysroot")
  set(runtime_definitions ${ELFCONV_RUNTIME_DEFINITIONS} -DTARGET_IS_WASI=1 "-DELFNAME=\"${ELFCONV_NAME}\"")
  set(wasi_flags -O3 "--sysroot=${wasi_sysroot}" -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_MMAN ${ELFCONV_RUNTIME_INCLUDE_FLAGS} -fno-exceptions)
  set(main_object "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.wasi.o")
  if(NOT ELFCONV_NO_COMPILED)
    _elfconv_execute("WASI bitcode compilation" "${wasi_cxx}" ${wasi_flags} ${runtime_definitions} -c "${main_ir}" -o "${main_object}")
  elseif(NOT EXISTS "${main_object}")
    message(FATAL_ERROR "ELFCONV_NO_COMPILED requires ${main_object}")
  endif()
  set(output_wasm "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.wasm")
  _elfconv_execute("WASI Wasm link" "${wasi_cxx}" ${wasi_flags} ${runtime_definitions}
    -lwasi-emulated-process-clocks -lwasi-emulated-mman -lwasi-emulated-signal
    -o "${output_wasm}" "${main_object}" ${ELFCONV_COMMON_RUNTIME_SOURCES}
    "${ELFCONV_RUNTIME_DIR}/syscalls/SyscallWasi.cpp"
  )
  if(NOT DEFINED ELFCONV_WASMEDGE OR ELFCONV_WASMEDGE STREQUAL "")
    find_program(ELFCONV_WASMEDGE NAMES wasmedge REQUIRED)
  endif()
  _elfconv_execute("WASI Wasm optimization" "${ELFCONV_WASMEDGE}" compile --optimize 3 "${output_wasm}" "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}_o3.wasm")
  return()
endif()

message(FATAL_ERROR "No conversion implementation for ELFCONV_TARGET=${ELFCONV_TARGET}")
