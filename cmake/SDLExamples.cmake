include_guard(GLOBAL)

function(elfconv_add_sdl_example)
  set(options PTHREADS INCREMENTAL_UNITS)
  set(one_value_args NAME GL_MODE SMOKE_FRAMES)
  set(multi_value_args SOURCES HEADERS)
  cmake_parse_arguments(PARSE_ARGV 0 SDL "${options}" "${one_value_args}" "${multi_value_args}")
  if(SDL_UNPARSED_ARGUMENTS OR NOT SDL_NAME OR NOT SDL_SOURCES OR
     NOT SDL_GL_MODE MATCHES "^(COMPATIBILITY|SHADER)$" OR NOT SDL_SMOKE_FRAMES)
    message(FATAL_ERROR "elfconv_add_sdl_example requires NAME, SOURCES, GL_MODE, and SMOKE_FRAMES")
  endif()
  if(NOT DEFINED ELFCONV_ROOT)
    get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  endif()
  if(NOT COMMAND elfconv_add_conversion_target)
    include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")
  endif()
  if(NOT ELFCONV_EMCC)
    set(ELFCONV_EMCC em++)
  endif()
  if(NOT ELFCONV_EMCC_C)
    set(ELFCONV_EMCC_C emcc)
  endif()
  set(_unset_pkg_config_libdir FALSE)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED ENV{PKG_CONFIG_LIBDIR}
     AND EXISTS "/usr/lib/i386-linux-gnu/pkgconfig")
    set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig")
    set(_unset_pkg_config_libdir TRUE)
  endif()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL_GL QUIET IMPORTED_TARGET sdl2 gl)
  endif()
  if(_unset_pkg_config_libdir)
    unset(ENV{PKG_CONFIG_LIBDIR})
  endif()
  if(NOT TARGET PkgConfig::SDL2)
    add_library(PkgConfig::SDL2 INTERFACE IMPORTED GLOBAL)
    set_property(TARGET PkgConfig::SDL2 PROPERTY INTERFACE_LINK_LIBRARIES SDL2)
  endif()
  if(NOT TARGET PkgConfig::SDL_GL)
    add_library(PkgConfig::SDL_GL INTERFACE IMPORTED GLOBAL)
    set_property(TARGET PkgConfig::SDL_GL PROPERTY INTERFACE_LINK_LIBRARIES "SDL2;GL")
  endif()
  set(_ordinary_sdl PkgConfig::SDL_GL)
  if(SDL_GL_MODE STREQUAL "COMPATIBILITY")
    set(_legacy_gl 1)
  else()
    set(_legacy_gl 0)
  endif()
  if(SDL_PTHREADS)
    find_package(Threads QUIET)
    if(NOT TARGET Threads::Threads)
      add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)
      set_property(TARGET Threads::Threads PROPERTY INTERFACE_COMPILE_OPTIONS -pthread)
      set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_OPTIONS -pthread)
    endif()
  endif()

  set(_source_paths)
  foreach(source IN LISTS SDL_SOURCES)
    list(APPEND _source_paths "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
  endforeach()
  set(_header_paths)
  foreach(header IN LISTS SDL_HEADERS)
    list(APPEND _header_paths "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
  endforeach()
  set(_project_paths ${_source_paths} ${_header_paths})
  set(_base "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${SDL_NAME}")
  set(_ordinary_dir "${_base}/i386")
  set(_wasm_dir "${_base}/wasm")
  set(_converted_dir "${_base}/i386-to-wasm")
  file(MAKE_DIRECTORY "${_ordinary_dir}" "${_wasm_dir}" "${_converted_dir}")

  set(_target "${SDL_NAME}-i386")
  set(_sources ${_project_paths})
  set(_compile_options -m32 -fno-pie)
  if(SDL_GL_MODE STREQUAL "SHADER")
    list(APPEND _compile_options -msse2 -mfpmath=sse)
  endif()
  if(SDL_PTHREADS)
    list(APPEND _compile_options -pthread)
  endif()
  add_executable("${_target}" EXCLUDE_FROM_ALL ${_sources})
  target_link_libraries("${_target}" PRIVATE ${_ordinary_sdl})
  if(SDL_PTHREADS)
    target_link_libraries("${_target}" PRIVATE Threads::Threads)
  endif()
  set_target_properties("${_target}" PROPERTIES
    OUTPUT_NAME "${SDL_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "${_ordinary_dir}")
  target_compile_features("${_target}" PRIVATE c_std_11)
  target_compile_options("${_target}" PRIVATE -O2 -g -Wall -Wextra ${_compile_options})
  target_link_options("${_target}" PRIVATE -m32 -fno-pie -no-pie)
  if(SDL_INCREMENTAL_UNITS)
    target_link_options("${_target}" PRIVATE "-Wl,-Map,${_ordinary_dir}/${SDL_NAME}.map")
  endif()

  set(_target "${SDL_NAME}-wasm")
  set(_output_dir "${_wasm_dir}")
  set(_sources ${_source_paths})
  set(_flags -O2 -std=c11 -sUSE_SDL=2)
  if(SDL_GL_MODE STREQUAL "COMPATIBILITY")
    list(APPEND _flags -sLEGACY_GL_EMULATION=1 -sASYNCIFY)
  else()
    list(APPEND _flags -sFULL_ES2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2)
  endif()
  if(SDL_PTHREADS)
    list(APPEND _flags -pthread -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=2
      -sDEFAULT_PTHREAD_STACK_SIZE=2097152 -sOFFSCREENCANVAS_SUPPORT=1
      "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas")
  endif()
  set(_html "${_output_dir}/${SDL_NAME}-wasm.html")
  set(ELFCONV_SDL_TARGET "${_target}")
  set(_shell "${_output_dir}/shell.html")
  configure_file("${ELFCONV_ROOT}/src/browser/sdl.html.in" "${_shell}" @ONLY)
  list(APPEND _flags --shell-file "${_shell}" -sEXIT_RUNTIME=1)
  add_custom_command(OUTPUT "${_html}"
    BYPRODUCTS "${_output_dir}/${SDL_NAME}-wasm.js"
      "${_output_dir}/${SDL_NAME}-wasm.wasm"
    COMMAND "${ELFCONV_EMCC_C}" ${_flags} ${_sources}
      -o "${_html}"
    DEPENDS ${_project_paths} ${_sources} "${_shell}"
    VERBATIM)
  add_custom_target("${_target}" DEPENDS "${_html}")

  file(GLOB _conversion_sources CONFIGURE_DEPENDS
    "${ELFCONV_ROOT}/src/runtime/*.cpp" "${ELFCONV_ROOT}/src/runtime/*.h"
    "${ELFCONV_ROOT}/src/runtime/*.inc" "${ELFCONV_ROOT}/src/utils/*.cpp"
    "${ELFCONV_ROOT}/src/utils/*.h" "${ELFCONV_ROOT}/scripts/*.py")
  list(APPEND _conversion_sources "${ELFCONV_ROOT}/src/browser/i386.html.in")

  set(_native_target "${SDL_NAME}-i386")
  set(_output_dir "${_converted_dir}")
  set(_elf_name "${SDL_NAME}-i386-to-wasm")
  set(_target "${_elf_name}")
  set(_link_map "${_ordinary_dir}/${SDL_NAME}.map")
  set(_object_base "${_ordinary_dir}")
  set(_conversion_options LEGACY_GL "${_legacy_gl}" JOBS 8
    OUTPUTS "${_output_dir}/${_elf_name}.html"
      "${_output_dir}/${_elf_name}.js" "${_output_dir}/${_elf_name}.wasm")
  if(SDL_INCREMENTAL_UNITS)
    list(APPEND _conversion_options INCREMENTAL_UNITS
      LINK_MAP "${_link_map}" OBJECT_BASE "${_object_base}" WASM_OPT_LEVEL 1)
  endif()
  if(SDL_PTHREADS)
    list(APPEND _conversion_options PTHREADS JSPI)
  endif()
  elfconv_add_conversion_target("${_target}"
    INPUT "$<TARGET_FILE:${_native_target}>"
    ELF_NAME "${_elf_name}"
    TARGET i386-wasm
    OUTPUT_DIR "${_output_dir}"
    DEPENDS "${_native_target}" ${_conversion_sources}
    ${_conversion_options})

  add_custom_target("${SDL_NAME}-smoke"
    COMMAND "$<TARGET_FILE:${SDL_NAME}-i386>" --frames "${SDL_SMOKE_FRAMES}"
    DEPENDS "${SDL_NAME}-i386"
    USES_TERMINAL VERBATIM)
  if(BUILD_TESTING)
    add_test(NAME "${SDL_NAME}-i386-smoke"
      COMMAND "$<TARGET_FILE:${SDL_NAME}-i386>" --frames "${SDL_SMOKE_FRAMES}")
  endif()
endfunction()
