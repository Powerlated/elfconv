cmake_minimum_required(VERSION 3.21)
project(sdl_triangle LANGUAGES C)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT DEFINED ENV{PKG_CONFIG_LIBDIR}
   AND EXISTS "/usr/lib/i386-linux-gnu/pkgconfig")
  set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig")
endif()
configure_file(index.html index.html COPYONLY)

include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL_GL REQUIRED IMPORTED_TARGET sdl2 gl)

add_executable(triangle triangle.c)
target_compile_features(triangle PRIVATE c_std_11)
target_compile_options(triangle PRIVATE -O2 -g -Wall -Wextra -m32 -fno-pie)
target_link_options(triangle PRIVATE -m32 -fno-pie -no-pie)
target_link_libraries(triangle PRIVATE PkgConfig::SDL_GL)

get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")
elfconv_add_conversion_target(triangle-wasm
  INPUT "$<TARGET_FILE:triangle>"
  ELF_NAME triangle
  TARGET i386-wasm
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS triangle
)

add_custom_target(triangle-smoke
  COMMAND "$<TARGET_FILE:triangle>" --frames 3
  DEPENDS triangle
  USES_TERMINAL
  VERBATIM
)
if(BUILD_TESTING)
  add_test(NAME triangle-smoke COMMAND "$<TARGET_FILE:triangle>" --frames 3)
endif()
