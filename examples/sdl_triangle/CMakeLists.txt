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

add_executable(triangle-i386 triangle.c)
set_target_properties(triangle-i386 PROPERTIES OUTPUT_NAME triangle)
target_compile_features(triangle-i386 PRIVATE c_std_11)
target_compile_options(triangle-i386 PRIVATE -O2 -g -Wall -Wextra -m32 -fno-pie)
target_link_options(triangle-i386 PRIVATE -m32 -fno-pie -no-pie)
target_link_libraries(triangle-i386 PRIVATE PkgConfig::SDL_GL)

get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")
elfconv_add_conversion_target(triangle-i386-to-wasm
  INPUT "$<TARGET_FILE:triangle-i386>"
  ELF_NAME triangle
  TARGET i386-wasm
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS triangle-i386
)

set(triangle_wasm_html "${CMAKE_CURRENT_BINARY_DIR}/triangle-wasm.html")
add_custom_command(
  OUTPUT "${triangle_wasm_html}"
  COMMAND "${ELFCONV_EMCC_C}"
    "${CMAKE_CURRENT_SOURCE_DIR}/triangle.c"
    -O2 -std=c11 -sUSE_SDL=2 -sLEGACY_GL_EMULATION=1
    -sASYNCIFY -sALLOW_MEMORY_GROWTH=1
    -o "${triangle_wasm_html}"
  DEPENDS triangle.c
  VERBATIM
)
add_custom_target(triangle-wasm DEPENDS "${triangle_wasm_html}")

add_custom_target(triangle-smoke
  COMMAND "$<TARGET_FILE:triangle-i386>" --frames 3
  DEPENDS triangle-i386
  USES_TERMINAL
  VERBATIM
)

if(BUILD_TESTING)
  add_test(NAME triangle-smoke COMMAND "$<TARGET_FILE:triangle-i386>" --frames 3)
endif()
