cmake_minimum_required(VERSION 3.14)
project(elfconv_test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/f8d7d77c06936315286eb55f8de22cd23c188571.zip
)

FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

if(CMAKE_ELFCONV_AARCH64_BUILD)
  add_executable(elfconv_test ../../src/utils/Util.cpp Run.cpp)
  target_link_libraries(elfconv_test PRIVATE GTest::gtest_main)
  target_include_directories(elfconv_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
  gtest_discover_tests(elfconv_test)
endif()

add_executable(i386_imports_test I386ImportsTests.cpp)

target_link_libraries(
  i386_imports_test
  GTest::gtest_main
)

target_include_directories(i386_imports_test PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/backend/remill/include
)
target_compile_definitions(i386_imports_test PRIVATE ELF_IS_I386 ADDRESS_SIZE_BITS=32)
target_compile_options(i386_imports_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(i386_imports_test PRIVATE -Wl,--gc-sections)

gtest_discover_tests(i386_imports_test)

add_executable(i386_tls_test I386TlsTests.cpp)
find_package(Threads REQUIRED)
target_link_libraries(i386_tls_test GTest::gtest_main Threads::Threads)
target_include_directories(i386_tls_test PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/backend/remill/include
)
target_compile_definitions(i386_tls_test PRIVATE ELF_IS_I386 ADDRESS_SIZE_BITS=32)
target_compile_options(i386_tls_test PRIVATE -ffunction-sections -fdata-sections)
target_link_options(i386_tls_test PRIVATE -Wl,--gc-sections)
gtest_discover_tests(i386_tls_test)

add_subdirectory(qemu-i386)
add_subdirectory(i386)
