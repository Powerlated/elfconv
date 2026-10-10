get_filename_component(ELFCONV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${ELFCONV_ROOT}/cmake/ElfconvTargets.cmake")

set(hello_output "${CMAKE_CURRENT_BINARY_DIR}/wasm-out")
set(bash_output "${CMAKE_CURRENT_BINARY_DIR}/wasm-out-bash")
set(bash_input "/usr/bin/bash-static" CACHE FILEPATH "Static Bash ELF for browser conversion")
set(busybox_input "${ELFCONV_ROOT}/examples/examples-repos/busybox/busybox" CACHE FILEPATH "BusyBox ELF for browser conversion")

elfconv_add_conversion_target(browser-hello
  INPUT "${CMAKE_BINARY_DIR}/examples/hello/c/hello_stripped"
  TARGET aarch64-wasm
  OUTPUT_DIR "${hello_output}"
  INIT_WASM
  DEPENDS hello-stripped
)

elfconv_add_conversion_target(browser-bash-init
  INPUT "${bash_input}"
  TARGET aarch64-wasm
  OUTPUT_DIR "${bash_output}"
  INIT_WASM
)
elfconv_add_conversion_target(browser-busybox
  INPUT "${busybox_input}"
  TARGET aarch64-wasm
  OUTPUT_DIR "${bash_output}"
  DEPENDS browser-bash-init
)
add_custom_target(browser-bash
  COMMAND "${CMAKE_COMMAND}"
    "-DELFCONV_PACK_PRELOAD_ONLY=ON"
    "-DELFCONV_MOUNT_SETTINGS=${CMAKE_CURRENT_LIST_DIR}/fixtures/testdir@/mnt/test"
    "-DELFCONV_OUTPUT_DIR=${bash_output}"
    -P "${ELFCONV_ROOT}/cmake/PackPreload.cmake"
  USES_TERMINAL
  VERBATIM
)
add_dependencies(browser-bash browser-busybox)
