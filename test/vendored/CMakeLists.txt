# All vendored i386 oracle registration is routed through this entry.
# The parent i386 test scope supplies Python3_EXECUTABLE and toolchain variables.
add_subdirectory(glibc/libc)
add_subdirectory(glibc/loader)
add_subdirectory(glibc/filesystem)
add_subdirectory(glibc/pthread)
add_subdirectory(sdl2)
