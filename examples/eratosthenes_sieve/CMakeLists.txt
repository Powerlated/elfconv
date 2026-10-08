cmake_minimum_required(VERSION 3.21)
project(eratosthenes_sieve_elf LANGUAGES C ASM)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ExampleTargets.cmake")

add_executable(eratosthenes_sieve_aarch64 start_aarch64.s cal.c)
target_link_options(eratosthenes_sieve_aarch64 PRIVATE -nostdlib)
elfconv_configure_arch_target(eratosthenes_sieve_aarch64 aarch64)
set_target_properties(eratosthenes_sieve_aarch64 PROPERTIES OUTPUT_NAME a SUFFIX ".aarch64")

add_executable(eratosthenes_sieve_amd64 start_amd64.s cal.c)
target_link_options(eratosthenes_sieve_amd64 PRIVATE -nostdlib)
elfconv_configure_arch_target(eratosthenes_sieve_amd64 amd64)
set_target_properties(eratosthenes_sieve_amd64 PROPERTIES OUTPUT_NAME a SUFFIX ".amd64")
