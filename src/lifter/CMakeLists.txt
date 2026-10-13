# Copyright (c) 2017 Trail of Bits, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

enable_language(ASM)

find_package(LLVM CONFIG REQUIRED)
find_path(BFD_INCLUDE_DIR NAMES bfd.h REQUIRED)
find_path(ELF_INCLUDE_DIR NAMES libelf.h REQUIRED)
find_path(DWARF_INCLUDE_DIR NAMES libdwarf/libdwarf.h REQUIRED)
find_library(BFD_LIBRARY NAMES bfd REQUIRED)
find_library(ELF_LIBRARY NAMES elf REQUIRED)
find_library(DWARF_LIBRARY NAMES dwarf REQUIRED)

add_executable(elflift
  Binary/Loader.cpp
  Binary/DynamicLibraries.cpp
  MainLifter.cpp
  TraceManager.cpp
  Lift.cpp
  "${ELFCONV_ROOT}/src/utils/Util.cpp"
)
add_dependencies(elflift semantics)
target_include_directories(elflift BEFORE PRIVATE "${DWARF_INCLUDE_DIR}")

set_target_properties(elflift PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(elflift PRIVATE -O3)

if(CMAKE_ELFCONV_AARCH64_BUILD)
  target_compile_definitions(elflift PRIVATE "ELFCONV_AARCH64_BUILD=1")
elseif(CMAKE_ELFCONV_X86_BUILD)
  target_compile_definitions(elflift PRIVATE "ELFCONV_X86_BUILD=1")
else()
  message(FATAL_ERROR "CMAKE_ELFCONV_<arch_name>_BUILD must be enabled (src/lifter/CMakeLists.txt)")
endif()

if(CMAKE_ELFLIFT_STATIC_LINK)
  find_library(LIBIBERTY_LIBRARY NAMES iberty REQUIRED)
  target_link_options(elflift PRIVATE -static)
  target_link_libraries(elflift PRIVATE bfd elf dwarf "${LIBIBERTY_LIBRARY}")
else()
  target_link_libraries(elflift PRIVATE
    "${BFD_LIBRARY}"
    "${ELF_LIBRARY}"
    "${DWARF_LIBRARY}"
  )
endif()

target_link_libraries(elflift PRIVATE remill ${PROJECT_LIBRARIES})
target_include_directories(elflift SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_include_directories(elflift PRIVATE
  "${BFD_INCLUDE_DIR}"
  "${ELF_INCLUDE_DIR}"
  "${DWARF_INCLUDE_DIR}"
  ${PROJECT_INCLUDEDIRECTORIES}
  ${ELFCONV_ROOT}
  "${ELFCONV_ROOT}/src"
)
install(TARGETS elflift RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(CMAKE_ELFCONV_X86_BUILD)
  if(NOT ELFCONV_EMCC)
    find_program(ELFCONV_EMCC NAMES em++)
  endif()
  if(ELFCONV_EMCC)
    get_filename_component(elfconv_emcc_dir "${ELFCONV_EMCC}" DIRECTORY)
    set(elfconv_gl4es_source_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/gl4es-src")
    set(elfconv_gl4es_build_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/gl4es-build")
    set(elfconv_gl4es_archive "${elfconv_gl4es_source_dir}/lib/libGL.a")
    include(ExternalProject)
    ExternalProject_Add(elfconv-gl4es-wasm
      URL "https://github.com/ptitSeb/gl4es/archive/ec16bedd8819c475326f4f1a3063772c6d986e06.tar.gz"
      URL_HASH SHA256=a6861d0976dd9808e0de09d6953724a20bb8de17add7313a88d500a56a6c4d79
      SOURCE_DIR "${elfconv_gl4es_source_dir}"
      PATCH_COMMAND patch --forward -p1 -i
        "${ELFCONV_ROOT}/src/lifter/gl4es-webgl2.patch"
      BINARY_DIR "${elfconv_gl4es_build_dir}"
      CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${elfconv_emcc_dir}/cmake/Modules/Platform/Emscripten.cmake"
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DNOX11:BOOL=ON
        -DNOEGL:BOOL=ON
        -DSTATICLIB:BOOL=ON
        -DDEFAULT_ES:STRING=3
        "-DCMAKE_C_FLAGS:STRING=-pthread -DELFCONV_WEBGL2=1"
      BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target GL --parallel 8
      INSTALL_COMMAND ""
      BUILD_BYPRODUCTS "${elfconv_gl4es_archive}"
      USES_TERMINAL_BUILD TRUE)
    if(EXISTS "${elfconv_emcc_dir}/emar")
      set(ELFCONV_EMAR "${elfconv_emcc_dir}/emar")
    else()
      find_program(ELFCONV_EMAR NAMES emar REQUIRED)
    endif()
    set(elfconv_runtime_sources
      "${ELFCONV_ROOT}/src/runtime/Entry.cpp"
      "${ELFCONV_ROOT}/src/runtime/Memory.cpp"
      "${ELFCONV_ROOT}/src/runtime/Runtime.cpp"
      "${ELFCONV_ROOT}/src/runtime/VmIntrinsics.cpp"
      "${ELFCONV_ROOT}/src/utils/Util.cpp"
      "${ELFCONV_ROOT}/src/utils/elfconv.cpp"
      "${ELFCONV_ROOT}/src/runtime/I386Imports.cpp")

    function(elfconv_add_runtime_archive suffix thread_flag incremental legacy_gl use_gl4es)
      set(runtime_objects)
      set(runtime_object_dir "${CMAKE_CURRENT_BINARY_DIR}/runtime-i386-wasm${suffix}")
      set(runtime_mode_flags)
      if(incremental)
        list(APPEND runtime_mode_flags -DELFCONV_INCREMENTAL_UNITS=1)
      endif()
      set(runtime_graphics_flags -DECV_USE_GL4ES=0)
      if(use_gl4es)
        set(runtime_graphics_flags -DECV_USE_GL4ES=1
          "-I${elfconv_gl4es_source_dir}/include")
      endif()
      foreach(source IN LISTS elfconv_runtime_sources)
        set(runtime_dependencies "${source}")
        if(use_gl4es)
          list(APPEND runtime_dependencies "${elfconv_gl4es_archive}")
        endif()
        get_filename_component(stem "${source}" NAME_WE)
        set(object "${runtime_object_dir}/${stem}.o")
        add_custom_command(OUTPUT "${object}"
          COMMAND "${CMAKE_COMMAND}" -E make_directory "${runtime_object_dir}"
          COMMAND "${ELFCONV_EMCC}" -O2 -fwasm-exceptions ${thread_flag}
            "-I${ELFCONV_ROOT}/backend/remill/include"
            "-I${ELFCONV_ROOT}" "-I${ELFCONV_ROOT}/src"
            -std=c++17 -DELF_IS_I386 -DADDRESS_SIZE_BITS=32 "-DECV_LEGACY_GL=${legacy_gl}"
            ${runtime_graphics_flags} ${runtime_mode_flags} -sUSE_SDL=2 -MMD -MF "${object}.d"
            -c "${source}" -o "${object}"
          DEPENDS ${runtime_dependencies}
          DEPFILE "${object}.d"
          VERBATIM)
        list(APPEND runtime_objects "${object}")
      endforeach()
      set(archive "${CMAKE_CURRENT_BINARY_DIR}/libelfconv-runtime-i386-wasm${suffix}.a")
      add_custom_command(OUTPUT "${archive}"
        COMMAND "${ELFCONV_EMAR}" rcs "${archive}" ${runtime_objects}
        DEPENDS ${runtime_objects}
        VERBATIM)
      add_custom_target(elfconv-runtime-i386-wasm${suffix} DEPENDS "${archive}")
      if(use_gl4es)
        add_dependencies(elfconv-runtime-i386-wasm${suffix} elfconv-gl4es-wasm)
      endif()
      set(elfconv_runtime_archives ${elfconv_runtime_archives} "${archive}" PARENT_SCOPE)
    endfunction()

    elfconv_add_runtime_archive("" "" OFF 0 0)
    elfconv_add_runtime_archive("-pthreads" "-pthread" OFF 0 0)
    elfconv_add_runtime_archive("-incremental" "" ON 0 0)
    elfconv_add_runtime_archive("-incremental-pthreads" "-pthread" ON 0 0)
    elfconv_add_runtime_archive("-incremental-pthreads-legacygl" "-pthread" ON 1 1)
    add_custom_target(elfconv_i386_wasm_runtime ALL DEPENDS ${elfconv_runtime_archives})
    install(FILES ${elfconv_runtime_archives} DESTINATION "${CMAKE_INSTALL_BINDIR}")
  elseif(ELFCONV_I386_TESTS)
    message(FATAL_ERROR "ELFCONV_I386_TESTS requires Emscripten to build the i386 Wasm runtime archives")
  endif()
endif()

