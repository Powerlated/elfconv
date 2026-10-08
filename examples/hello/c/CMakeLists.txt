cmake_minimum_required(VERSION 3.21)
project(hello_c LANGUAGES C)

add_executable(hello hello.c)
target_link_options(hello PRIVATE -static)

set(stripped_hello "${CMAKE_CURRENT_BINARY_DIR}/hello_stripped")
add_custom_command(
  OUTPUT "${stripped_hello}"
  COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:hello>" -o "${stripped_hello}"
  DEPENDS hello
  VERBATIM
)
add_custom_target(hello-stripped DEPENDS "${stripped_hello}")
