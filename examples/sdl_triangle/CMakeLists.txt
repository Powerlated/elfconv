cmake_minimum_required(VERSION 3.21)
project(sdl_triangle LANGUAGES C CXX)

include(CTest)
get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")
include("${ELFCONV_ROOT}/cmake/SDLExamples.cmake")

elfconv_add_sdl_example(
  NAME sdl_triangle
  SOURCES triangle.c
  GL_MODE COMPATIBILITY
  SMOKE_FRAMES 3)
