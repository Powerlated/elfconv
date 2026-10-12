cmake_minimum_required(VERSION 3.21)
project(sdl_cube_pthread LANGUAGES C)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT DEFINED ENV{PKG_CONFIG_LIBDIR}
   AND EXISTS "/usr/lib/i386-linux-gnu/pkgconfig")
  set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig")
endif()
configure_file(index.html index.html COPYONLY)

include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDL_GL REQUIRED IMPORTED_TARGET sdl2 gl)

add_executable(cube-i386 cube.c cube_geometry.c cube_shader.c cube_render.c)
set_target_properties(cube-i386 PROPERTIES OUTPUT_NAME cube)
target_compile_features(cube-i386 PRIVATE c_std_11)
target_compile_options(cube-i386 PRIVATE -O2 -g -Wall -Wextra -m32 -msse2 -mfpmath=sse -fno-pie)
target_link_options(cube-i386 PRIVATE -m32 -fno-pie -no-pie)
target_link_options(cube-i386 PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/cube.map")
target_link_libraries(cube-i386 PRIVATE PkgConfig::SDL_GL Threads::Threads)

get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")
elfconv_add_conversion_target(cube-i386-to-wasm
  INPUT "$<TARGET_FILE:cube-i386>"
  ELF_NAME cube
  TARGET i386-wasm
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
  LEGACY_GL 0
  INCREMENTAL_UNITS
  PTHREADS
  LINK_MAP "${CMAKE_CURRENT_BINARY_DIR}/cube.map"
  OBJECT_BASE "${CMAKE_CURRENT_BINARY_DIR}"
  WASM_OPT_LEVEL 1
  DEPENDS cube-i386
)

add_custom_target(cube-smoke
  COMMAND "$<TARGET_FILE:cube-i386>" --frames 90
  DEPENDS cube-i386
  USES_TERMINAL
  VERBATIM
)
if(BUILD_TESTING)
  add_test(NAME cube-smoke COMMAND "$<TARGET_FILE:cube-i386>" --frames 90)
endif()
