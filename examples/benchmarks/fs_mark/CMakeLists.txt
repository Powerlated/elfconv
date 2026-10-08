cmake_minimum_required(VERSION 3.21)
project(fs_mark LANGUAGES C)

set(FS_MARK_DIR1 "/test/dir1" CACHE PATH "First fs_mark test directory")
set(FS_MARK_DIR2 "/test/dir2" CACHE PATH "Second fs_mark test directory")
set(WASI_SDK_PATH "$ENV{WASI_SDK_PATH}" CACHE PATH "WASI SDK root")

add_executable(fs_mark fs_mark.c lib_timing.c)
target_compile_options(fs_mark PRIVATE -O3 -Wall -D_FILE_OFFSET_BITS=64)
target_link_options(fs_mark PRIVATE -static)

add_executable(fs_mark_tmp wasi_fs_mark.c lib_timing.c)
target_compile_options(fs_mark_tmp PRIVATE -O3 -Wall -D_FILE_OFFSET_BITS=64)
target_link_options(fs_mark_tmp PRIVATE -static)
set_target_properties(fs_mark_tmp PROPERTIES OUTPUT_NAME tmp_fs_mark SUFFIX ".aarch64")

add_custom_target(fs_mark-test
  COMMAND "$<TARGET_FILE:fs_mark>" -d "${FS_MARK_DIR1}" -d "${FS_MARK_DIR2}" -s 51200 -n 4096
  COMMAND "$<TARGET_FILE:fs_mark>" -d "${FS_MARK_DIR1}" -d "${FS_MARK_DIR2}" -s 51200 -n 4096 -r
  COMMAND "$<TARGET_FILE:fs_mark>" -d "${FS_MARK_DIR1}" -d "${FS_MARK_DIR2}" -s 51200 -n 4096 -D 128
  COMMAND "$<TARGET_FILE:fs_mark>" -d "${FS_MARK_DIR1}" -d "${FS_MARK_DIR2}" -s 51200 -n 4096 -r -D 128
  USES_TERMINAL
  VERBATIM
)
add_dependencies(fs_mark-test fs_mark)

if(WASI_SDK_PATH)
  set(wasi_output "${CMAKE_CURRENT_BINARY_DIR}/fs_mark_wasi.wasm")
  add_custom_command(
    OUTPUT "${wasi_output}"
    COMMAND "${WASI_SDK_PATH}/bin/clang" -O3
      "${CMAKE_CURRENT_SOURCE_DIR}/wasi_fs_mark.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/lib_timing.c"
      -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID
      -lwasi-emulated-process-clocks -lwasi-emulated-getpid
      -o "${wasi_output}"
    DEPENDS wasi_fs_mark.c lib_timing.c fs_mark.h
    VERBATIM
  )
  add_custom_target(fs_mark_wasi DEPENDS "${wasi_output}")
endif()
