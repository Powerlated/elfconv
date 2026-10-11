include(ExternalProject)

if(NOT ELFCONV_I386_TESTS)
  return()
endif()

if(NOT DEFINED ELFCONV_I386_CC OR NOT DEFINED ELFCONV_I386_NODE OR
   NOT DEFINED ELFCONV_I386_OBJCOPY OR NOT DEFINED ELFCONV_EMCC)
  message(FATAL_ERROR "SDL2 i386 oracle requires the toolchain variables from test/elfconv/i386/CMakeLists.txt")
endif()

set(SDL2_PIN fa24d868ac2f8fd558e4e914c9863411245db8fd)
set(SDL2_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/sdl2-i386-install")
set(SDL2_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/sdl2-source")
set(SDL2_BUILD "${CMAKE_CURRENT_BINARY_DIR}/sdl2-i386-build")
ExternalProject_Add(sdl2_i386_upstream
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG "${SDL2_PIN}"
  UPDATE_DISCONNECTED TRUE
  SOURCE_DIR "${SDL2_SOURCE}"
  BINARY_DIR "${SDL2_BUILD}"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=${ELFCONV_I386_CC}
    -DCMAKE_C_FLAGS=-m32
    -DCMAKE_EXE_LINKER_FLAGS=-m32
    -DCMAKE_SHARED_LINKER_FLAGS=-m32
    -DCMAKE_INSTALL_PREFIX=${SDL2_PREFIX}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DSDL_SHARED=ON
    -DSDL_STATIC=OFF
    -DSDL_TEST=ON
    -DSDL_TESTS=OFF
    -DSDL_AUDIO=OFF
    -DSDL_VIDEO=OFF
    -DSDL_RENDER=OFF
    -DSDL_JOYSTICK=OFF
    -DSDL_HAPTIC=OFF
    -DSDL_SENSOR=OFF
    -DSDL_POWER=OFF
    -DSDL_HIDAPI=OFF
    -DSDL_INSTALL_TESTS=OFF
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config Release
  BUILD_BYPRODUCTS "${SDL2_PREFIX}/lib/libSDL2_test.a")

add_custom_target(sdl2_surface_pixel_prerequisite ALL
  DEPENDS sdl2_i386_upstream)

set(SDL2_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/test")
add_test(NAME i386_sdl2_surface_pixels
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../elfconv/run_i386.py"
    --lifter "$<TARGET_FILE:elflift>"
    --emcc "${ELFCONV_EMCC}"
    --cc "${ELFCONV_I386_CC}"
    --objcopy "${ELFCONV_I386_OBJCOPY}"
    --node "${ELFCONV_I386_NODE}"
    --source "${SDL2_TEST_SOURCE}/harness.c"
    --extra-source "${SDL2_TEST_SOURCE}/testautomation_pixels.c"
    --extra-source "${SDL2_TEST_SOURCE}/testautomation_surface.c"
    --compile-flag=-I${SDL2_PREFIX}/include/SDL2
    --link-flag=-L${SDL2_PREFIX}/lib
    --link-flag=-Wl,-rpath,${SDL2_PREFIX}/lib
    --link-flag=-lSDL2_test
    --link-flag=-lSDL2
    --runtime-library-path "${SDL2_PREFIX}/lib"
    --output-dir "${CMAKE_CURRENT_BINARY_DIR}/surface-pixels")
set_tests_properties(i386_sdl2_surface_pixels PROPERTIES
  LABELS "i386;regression;sdl2"
  TIMEOUT 1200
  RUN_SERIAL TRUE)
