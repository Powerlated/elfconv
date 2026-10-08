cmake_minimum_required(VERSION 3.21)
project(dhrystone LANGUAGES C)

add_executable(dhrystone dhrystone.c)
set_target_properties(dhrystone PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(dhrystone PRIVATE -O3)
target_link_options(dhrystone PRIVATE -static)
