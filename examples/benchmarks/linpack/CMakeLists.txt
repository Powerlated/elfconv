cmake_minimum_required(VERSION 3.21)
project(linpack LANGUAGES C)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ExampleTargets.cmake")

add_executable(linpack nostdin_linpack.c)
target_compile_options(linpack PRIVATE -O3 -pthread)
target_link_options(linpack PRIVATE -static -pthread)
elfconv_configure_arch_target(linpack aarch64)
set_target_properties(linpack PROPERTIES OUTPUT_NAME a SUFFIX ".aarch64")

set(stripped_linpack "${CMAKE_CURRENT_BINARY_DIR}/a_stripped.aarch64")
add_custom_command(
  OUTPUT "${stripped_linpack}"
  COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:linpack>" -o "${stripped_linpack}"
  DEPENDS linpack
  VERBATIM
)
add_custom_target(linpack-stripped DEPENDS "${stripped_linpack}")
