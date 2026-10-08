cmake_minimum_required(VERSION 3.21)
project(mini_examples LANGUAGES C ASM)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ExampleTargets.cmake")

add_executable(simple_hello_world_aarch64 simple_hello_world_aarch64.s)
target_link_options(simple_hello_world_aarch64 PRIVATE -nostdlib)
elfconv_configure_arch_target(simple_hello_world_aarch64 aarch64)
set_target_properties(simple_hello_world_aarch64 PROPERTIES OUTPUT_NAME a SUFFIX ".aarch64")

add_executable(simple_hello_world_amd64 simple_hello_world_amd64.s)
target_link_options(simple_hello_world_amd64 PRIVATE -nostdlib)
elfconv_configure_arch_target(simple_hello_world_amd64 amd64)
set_target_properties(simple_hello_world_amd64 PROPERTIES OUTPUT_NAME a SUFFIX ".amd64")

add_executable(amd64_test amd64_test.s)
target_link_options(amd64_test PRIVATE -nostdlib)
elfconv_configure_arch_target(amd64_test amd64)
set_target_properties(amd64_test PROPERTIES SUFFIX ".amd64")
