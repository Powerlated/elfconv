cmake_minimum_required(VERSION 3.21)
project(sdl_cube LANGUAGES C CXX)

include(CTest)
get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")
include("${ELFCONV_ROOT}/cmake/SDLExamples.cmake")

elfconv_add_sdl_example(
  NAME sdl_cube
  SOURCES cube.c cube_geometry.c cube_shader.c cube_render.c
  HEADERS cube.h
  GL_MODE SHADER
  SMOKE_FRAMES 90
  INCREMENTAL_UNITS)
