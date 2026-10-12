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
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
foreach(case IN ITEMS cache failure cancellation preload chunk_pool)
  add_test(NAME conversion_driver_${case}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ConversionDriverTests.py" "${case}")
  set_tests_properties(conversion_driver_${case} PROPERTIES TIMEOUT 15)
endforeach()
add_test(NAME conversion_driver_compiler
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ConversionDriverTests.py"
    compiler "${CMAKE_CXX_COMPILER}")
set_tests_properties(conversion_driver_compiler PROPERTIES TIMEOUT 30)

if(CMAKE_ELFCONV_AARCH64_BUILD)
  add_executable(elfconv_test ../../src/utils/Util.cpp Run.cpp)
  target_link_libraries(elfconv_test PRIVATE GTest::gtest_main)
  target_include_directories(elfconv_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
  gtest_discover_tests(elfconv_test)
endif()

add_executable(llvm_migration_test LlvmMigrationTests.cpp)
target_include_directories(llvm_migration_test BEFORE PRIVATE
  "${googletest_SOURCE_DIR}/googletest/include")
target_link_libraries(llvm_migration_test PRIVATE remill GTest::gtest_main)
gtest_discover_tests(llvm_migration_test)
if(CMAKE_ELFCONV_X86_BUILD)
  add_dependencies(llvm_migration_test x86)
  target_compile_definitions(llvm_migration_test PRIVATE
    ELFCONV_TEST_SEMANTICS="${CMAKE_BINARY_DIR}/backend/remill/lib/Arch/X86/Runtime/x86.bc")
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
