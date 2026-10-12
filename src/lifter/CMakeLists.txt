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
    file(GLOB_RECURSE elfconv_runtime_headers CONFIGURE_DEPENDS
      "${ELFCONV_ROOT}/src/runtime/*.h"
      "${ELFCONV_ROOT}/src/runtime/*.hpp"
      "${ELFCONV_ROOT}/src/runtime/*.inc"
      "${ELFCONV_ROOT}/src/utils/*.h"
      "${ELFCONV_ROOT}/src/utils/*.hpp"
      "${ELFCONV_ROOT}/src/utils/*.inc"
      "${ELFCONV_ROOT}/backend/remill/include/*.h"
      "${ELFCONV_ROOT}/backend/remill/include/*.hpp"
      "${ELFCONV_ROOT}/backend/remill/include/*.inc")

    function(elfconv_add_runtime_archive suffix thread_flag incremental)
      set(runtime_objects)
      set(runtime_object_dir "${CMAKE_CURRENT_BINARY_DIR}/runtime-i386-wasm${suffix}")
      set(runtime_mode_flags)
      if(incremental)
        list(APPEND runtime_mode_flags -DELFCONV_INCREMENTAL_UNITS=1)
      endif()
      foreach(source IN LISTS elfconv_runtime_sources)
        get_filename_component(stem "${source}" NAME_WE)
        set(object "${runtime_object_dir}/${stem}.o")
        add_custom_command(OUTPUT "${object}"
          COMMAND "${CMAKE_COMMAND}" -E make_directory "${runtime_object_dir}"
          COMMAND "${ELFCONV_EMCC}" -O2 -fwasm-exceptions ${thread_flag}
            "-I${ELFCONV_ROOT}/backend/remill/include"
            "-I${ELFCONV_ROOT}" "-I${ELFCONV_ROOT}/src"
            -std=c++17 -DELF_IS_I386 -DADDRESS_SIZE_BITS=32 -DECV_LEGACY_GL=0
            ${runtime_mode_flags} -sUSE_SDL=2 -c "${source}" -o "${object}"
          DEPENDS "${source}" ${elfconv_runtime_headers}
          VERBATIM)
        list(APPEND runtime_objects "${object}")
      endforeach()
      set(archive "${CMAKE_CURRENT_BINARY_DIR}/libelfconv-runtime-i386-wasm${suffix}.a")
      add_custom_command(OUTPUT "${archive}"
        COMMAND "${ELFCONV_EMAR}" rcs "${archive}" ${runtime_objects}
        DEPENDS ${runtime_objects}
        VERBATIM)
      set(elfconv_runtime_archives ${elfconv_runtime_archives} "${archive}" PARENT_SCOPE)
    endfunction()

    elfconv_add_runtime_archive("" "" OFF)
    elfconv_add_runtime_archive("-pthreads" "-pthread" OFF)
    elfconv_add_runtime_archive("-incremental" "" ON)
    elfconv_add_runtime_archive("-incremental-pthreads" "-pthread" ON)
    add_custom_target(elfconv_i386_wasm_runtime ALL DEPENDS ${elfconv_runtime_archives})
    add_dependencies(elflift elfconv_i386_wasm_runtime)
    install(FILES ${elfconv_runtime_archives} DESTINATION "${CMAKE_INSTALL_BINDIR}")
  elseif(ELFCONV_I386_TESTS)
    message(FATAL_ERROR "ELFCONV_I386_TESTS requires Emscripten to build the i386 Wasm runtime archives")
  endif()
endif()

