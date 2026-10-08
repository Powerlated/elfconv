cmake_minimum_required(VERSION 3.21)
project(sdl_cube LANGUAGES C)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT DEFINED ENV{PKG_CONFIG_LIBDIR}
   AND EXISTS "/usr/lib/i386-linux-gnu/pkgconfig")
  set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig")
endif()
configure_file(index.html index.html COPYONLY)

include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL_GL REQUIRED IMPORTED_TARGET sdl2 gl)

add_executable(cube cube.c cube_geometry.c cube_shader.c cube_render.c)
target_compile_features(cube PRIVATE c_std_11)
target_compile_options(cube PRIVATE -O2 -g -Wall -Wextra -m32 -msse2 -mfpmath=sse -fno-pie)
target_link_options(cube PRIVATE -m32 -fno-pie -no-pie)
target_link_options(cube PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/cube.map")
target_link_libraries(cube PRIVATE PkgConfig::SDL_GL)

get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")
elfconv_add_conversion_target(cube-wasm
  INPUT "$<TARGET_FILE:cube>"
  ELF_NAME cube
  TARGET i386-wasm
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
  LEGACY_GL 0
  INCREMENTAL_UNITS
  LINK_MAP "${CMAKE_CURRENT_BINARY_DIR}/cube.map"
  OBJECT_BASE "${CMAKE_CURRENT_BINARY_DIR}"
  WASM_OPT_LEVEL 1
  DEPENDS cube
)

add_custom_target(cube-smoke
  COMMAND "$<TARGET_FILE:cube>" --frames 90
  DEPENDS cube
  USES_TERMINAL
  VERBATIM
)
if(BUILD_TESTING)
  add_test(NAME cube-smoke COMMAND "$<TARGET_FILE:cube>" --frames 90)
endif()
