cmake_minimum_required(VERSION 3.21)
project(systemcalls_multiprocesses LANGUAGES C)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ExampleTargets.cmake")

foreach(example IN ITEMS parent child)
  add_executable("${example}" "${example}.c")
  target_compile_options("${example}" PRIVATE -O3 -pthread)
  target_link_options("${example}" PRIVATE -pthread)
  elfconv_configure_arch_target("${example}" aarch64)
endforeach()
