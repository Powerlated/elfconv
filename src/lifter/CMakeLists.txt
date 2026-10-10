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

