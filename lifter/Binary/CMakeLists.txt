cmake_minimum_required(VERSION 3.21)
project(binary_loader_test LANGUAGES CXX)
get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GFLAGS_USE_TARGET_NAMESPACE ON)
find_package(gflags CONFIG REQUIRED)
find_package(glog CONFIG REQUIRED)

find_path(BFD_INCLUDE_DIR NAMES bfd.h REQUIRED)
find_path(ELF_INCLUDE_DIR NAMES libelf.h REQUIRED)
find_path(DWARF_INCLUDE_DIR NAMES libdwarf/libdwarf.h REQUIRED)
find_library(BFD_LIBRARY NAMES bfd REQUIRED)
find_library(ELF_LIBRARY NAMES elf REQUIRED)
find_library(DWARF_LIBRARY NAMES dwarf REQUIRED)
add_executable(binary-loader-test test.cpp Loader.cpp "${ELFCONV_ROOT}/utils/Util.cpp")
target_include_directories(binary-loader-test BEFORE PRIVATE "${DWARF_INCLUDE_DIR}")
target_include_directories(binary-loader-test PRIVATE
  "${BFD_INCLUDE_DIR}"
  "${ELF_INCLUDE_DIR}"
  "${DWARF_INCLUDE_DIR}"
  "${ELFCONV_ROOT}"
  "${ELFCONV_ROOT}/backend/remill/include"
)
target_link_libraries(binary-loader-test PRIVATE
  "${BFD_LIBRARY}"
  "${ELF_LIBRARY}"
  "${DWARF_LIBRARY}"
  glog::glog
)
set_target_properties(binary-loader-test PROPERTIES OUTPUT_NAME test)
