# Included by the parent test integration after SDL2's pinned i386 runtime and
# SDL2_test have been built/installed for the i386 compiler toolchain.
find_package(SDL2 2.0.23 REQUIRED COMPONENTS SDL2-static SDL2test)

add_executable(sdl2_surface_pixel_oracles
  test/harness.c
  test/testautomation_pixels.c
  test/testautomation_surface.c)
target_link_libraries(sdl2_surface_pixel_oracles PRIVATE
  SDL2::SDL2test
  SDL2::SDL2-static)
