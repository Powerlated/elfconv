cmake_minimum_required(VERSION 3.21)
project(benchmark_sieve LANGUAGES C)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ExampleTargets.cmake")

add_executable(eratosthenes_sieve main.c)
target_compile_options(eratosthenes_sieve PRIVATE -O3 -pthread)
target_link_options(eratosthenes_sieve PRIVATE -static -pthread)
elfconv_configure_arch_target(eratosthenes_sieve aarch64)
set_target_properties(eratosthenes_sieve PROPERTIES OUTPUT_NAME eratosthenes_sieve SUFFIX ".aarch64")
