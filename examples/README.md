# Examples for conversion

Each project-owned example uses its directory's `CMakeLists.txt`. Configure
from the repository root and build into `build/`.

## SDL / OpenGL 2 triangle

`sdl_triangle` builds a dynamically linked, non-PIE i386 ELF executable. It
requests an OpenGL 2.0 context and draws an RGB triangle using compatibility
calls. Building the ELF requires an i386 C toolchain and i386 SDL2/OpenGL
development libraries.

```bash
cmake -S examples/sdl_triangle -B build/sdl_triangle
cmake --build build/sdl_triangle --target triangle
cmake --build build/sdl_triangle --target triangle-smoke
```

Escape or closing the window exits interactive mode. `triangle-smoke` renders
three frames and checks that the triangle's center is colored and the background
is black. The target keeps `-m32 -fno-pie -no-pie` for conversion.

### Convert the ELF to Wasm

Build the x86-enabled elfconv lifter and install/configure Emscripten first.
Then configure and build the conversion target:

```bash
cmake -S examples/sdl_triangle -B build/sdl_triangle \
  -DELFCONV_LIFTER="$PWD/build/lifter/elflift" \
  -DELFCONV_EMCC="$HOME/emsdk/upstream/emscripten/em++"
cmake --build build/sdl_triangle --target triangle-wasm
python3 -m http.server 8000 --bind 127.0.0.1 \
  --directory build/sdl_triangle
```

Open `http://127.0.0.1:8000/` for interactive rendering, or
`http://127.0.0.1:8000/?frames=3` for framebuffer checks and an exit status.
`triangle-wasm` lifts `triangle` into `triangle.bc` and links it with the
host-import runtime to produce `triangle.js` and `triangle.wasm`. It does **not**
compile `triangle.c` with Emscripten. The build directory includes the copied
`index.html` used by the local server.

The converted browser smoke has passed with center RGB `63,64,128`, a black
background, exit status 0, and interactive Escape handling. SDL2 is supplied
by Emscripten; legacy OpenGL calls use its immediate-mode emulation. The runtime
adapts i386 stack arguments, opaque SDL handles, returned strings, varargs,
WebGL RGBA readback into desktop RGB/pack layout, and Asyncify yields at buffer
swaps so rendering and input remain responsive.

This path handles the triangle's imported functions and relocations, not
arbitrary Linux shared libraries or retail Portal. Unsupported relocations,
unknown host imports, unsupported pointer-bearing SDL events, and raw Linux
i386 syscalls fail explicitly. The printf adapters accept scalar `%s`, `%d`,
`%u`, `%c`, `%x`, `%X`, and `%%` conversions; floating-point/length-modified
varargs are not implemented. Guest readback supports RGB/RGBA unsigned bytes.
The SDL cube also resolves fortified printf, C23 `strtoul`, and stack-protector
imports.

If using a separate i386 sysroot, configure it through the normal CMake compiler
flags and pkg-config environment:

```bash
SYSROOT=/path/to/i386-sysroot
LLVM_ROOT=/path/to/llvm
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/i386-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
cmake -S examples/sdl_triangle -B build/sdl_triangle \
  -DCMAKE_C_COMPILER="$LLVM_ROOT/bin/clang" \
  -DCMAKE_C_FLAGS="--sysroot=$SYSROOT -I$SYSROOT/usr/include/i386-linux-gnu -B$SYSROOT/usr/lib/i386-linux-gnu" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$SYSROOT/usr/lib/gcc/i686-linux-gnu/15" \
  -DELFCONV_LIFTER="$PWD/build/lifter/elflift" \
  -DELFCONV_EMCC="$HOME/emsdk/upstream/emscripten/em++"
cmake --build build/sdl_triangle --target triangle-wasm
```

## SDL / OpenGL cube

`sdl_cube` uses the same i386 SDL2/OpenGL requirements. Its executable is split
across `cube.c`, `cube_geometry.c`, `cube_shader.c`, and `cube_render.c` so
`cube-wasm` exercises per-object incremental lifting and compilation.

```bash
cmake -S examples/sdl_cube -B build/sdl_cube
cmake --build build/sdl_cube --target cube
cmake --build build/sdl_cube --target cube-smoke
```

To convert and serve it, configure with the lifter and Emscripten paths, then
build `cube-wasm`:

```bash
cmake -S examples/sdl_cube -B build/sdl_cube \
  -DELFCONV_LIFTER="$PWD/build/lifter/elflift" \
  -DELFCONV_EMCC="$HOME/emsdk/upstream/emscripten/em++"
cmake --build build/sdl_cube --target cube-wasm
python3 -m http.server 8000 --bind 127.0.0.1 --directory build/sdl_cube
```

Open `http://127.0.0.1:8000/` to run the cube in a browser.
The converted cube smoke passed in Chromium with `?frames=90`: both framebuffer
readbacks passed, the center pixel changed between frames 1 and 90, the corner
remained black, and the guest exited with status 0. It uses shader/VBO OpenGL
calls without legacy GL emulation.

The cube target emits a GNU linker map, then partitions the final relocated ELF
by input-object code ranges. It caches each object's lifted bitcode and
independently optimized Wasm object under
`build/sdl_cube/.elfconv-incremental/`; shared ELF metadata and runtime objects
have separate cache entries. A no-op rebuild skips lifting, compilation, and
linking. Changing one source object rebuilds its unit, plus any units whose
VMAs move because of link layout changes. This uses final linked code rather
than raw relocatable `.o` files, preserving linker-applied relocations. The
final Wasm link remains whole-program and does not perform cross-unit LTO.


[`examples-repos`](https://github.com/yomaytk/elfconv/tree/main/examples/examples-repos) has patch or config files that can be used to convert the third-party programs. You can convert those Linux/ELF binaries following the steps below.
## [mnist-neural-network-plain-c](https://github.com/AndrewCarterUK/mnist-neural-network-plain-c)
- This is the neural network training MNIST dataset written by C. After applying patch file [`example-repos/mnist-neural-network-plain-c.patch`](https://github.com/yomaytk/elfconv/tree/main/examples/examples-repos/mnist-neural-network-plain-c.patch) (needed for configuring static linking and so on), you can convert the generated ELF binary.
```bash
$ git clone https://github.com/AndrewCarterUK/mnist-neural-network-plain-c
$ cd mnist-neural-network-plain-c
$ git apply path/to/mnist-neural-network-plain-c.patch
$ make # mnist.aarch64 is generated, and you can convert it.
```
## [busybox](https://github.com/mirror/busybox)
- This is a software suite that provides several Unix utilities in a single executable file. [wiki](https://en.wikipedia.org/wiki/BusyBox)
- We should set some configuration when we build this project. You can use [`examples-repos/.config`](https://github.com/yomaytk/elfconv/tree/main/examples/examples-repos/.config) for configuration.

```bash
$ git clone https://github.com/mirror/busybox
$ cd busybox
$ git checkout 1_36_stable # We've confirmed that at least 'v1_36_stable' works.
$ cp path/to/examples-repos/.config .config
$ make # busybox is generated, and you can convert it.
```
### sh-busybox
- We customize BusyBox to add a simple shell program (based on [`lsh`](https://github.com/brenns10/lsh)) that executes BusyBox in the browser, called `sh-busybox`. `sh-busybox` can be converted to a unit Wasm application, and you can try executing some busybox applet commands in the browser.
```bash
# same up to the `cp` command above.
$ git apply path/to/sh-busybox.patch
$ make # busybox is generated, and you can convert it and execute it on the browser.
```

## sm64ex

[`sm64ex`](https://github.com/sm64pc/sm64ex) is pinned as a submodule at
`examples/sm64ex`. Initialize it with:

```bash
git submodule update --init examples/sm64ex
```

The native build requires a user-supplied `baserom.us.z64` in that directory
to extract game assets. Do not commit the ROM or extracted copyrighted assets.

After configuring the repository build for `ELFCONV_ARCH=x86` and building its
dependencies, set the local i386 sysroot and Emscripten compiler, then build the
CMake targets from the repository root:

```bash
cmake -S . -B build \
  -DELFCONV_SM64EX_SYSROOT="$HOME/.local/opt/elfconv-i386-sysroot" \
  -DELFCONV_EMCC="$HOME/emsdk/upstream/emscripten/em++"
cmake --build build --target sm64ex-native
cmake --build build --target sm64ex-bitcode
cmake --build build --target sm64ex-wasm
```

On Debian/Ubuntu with the i386 development libraries installed on the host,
use `-DELFCONV_SM64EX_SYSROOT=/` instead of a separate sysroot. The native
build requires the i386 SDL2 and OpenGL development packages. CMake passes the
compiler path through `CC` and the sysroot flags through `PLATFORM_CFLAGS`;
upstream's recursive tool build requires `CC` to contain only the executable.

The CMake targets invoke the pinned upstream Makefiles to build SM64EX's asset
tools and game. The game is compiled as an i386 PIE ELF with `-fPIE -pie`;
outputs are under `examples/sm64ex/build/us_pc_pie/`, separate from old non-PIE objects.
The lifter applies a fixed `0x08000000` guest load bias to sections, function
addresses, initializer pointers, relative relocations, and linker-map ranges.
This is AOT PIE support, not ASLR or a general shared-library loader.

### Shared-library configuration

Enable `ELFCONV_SM64EX_SHARED` to build the main game as a PIC i386 `.so`
(`-fPIC -shared -Wl,-z,defs`) instead of a PIE executable:

```sh
cmake -S . -B build/llvm16 -DELFCONV_SM64EX_SHARED=ON
cmake --build build/llvm16 --target sm64ex-native
examples/sm64ex/build/us_pc_shared/sm64-runner \
  "$PWD/examples/sm64ex/build/us_pc_shared/sm64.us.f3dex2e.so" --help
cmake --build build/llvm16 --target sm64ex-wasm
python3 -m http.server 8080 --directory examples/sm64ex/build/us_pc_shared
```

Open `/sm64.us.f3dex2e.so.html` and click Start. The native runner uses
`dlopen`/`dlsym` to invoke exported `main`; conversion lifts the `.so` itself
with `ELFCONV_ENTRY_SYMBOL=main`, not the runner or source. Outputs and
incremental caches are isolated under `build/us_pc_shared/`.
Set `ELFCONV_SM64EX_SHARED=OFF` to return to the PIE configuration.

The shared-library build was lifted into 115 incremental units and exercised
in Chromium: Mario and the “PRESS START” title screen rendered with no browser
errors. The native `dlopen` runner also passed `--help`.

Shared-object conversion resolves `R_386_RELATIVE`, defined-symbol `R_386_32`,
`R_386_GLOB_DAT`, and `R_386_JMP_SLOT` at a fixed guest bias. ELF initializers
are read after relocation. Exported entries must have `int(int,char**)` ABI;
guest argc/argv are passed on the i386 stack and the return value becomes the
program exit status. This does not implement runtime Linux `dlopen`, dependency
loading, symbol interposition, TLS, or arbitrary host-facing export signatures.

### Runtime and incremental conversion

The native executable's `--help` command succeeds, and the lifted bitcode
passes LLVM verification. Lifting required fixes for `stdout` copy relocations,
variable-length x86 instruction scanning in indirect-jump functions, i386
indirect-branch PHI widths, and missing SIMD/NOP semantics.

x87 arithmetic now uses binary64 `double`, including math builtins and NaN
classification. The ten-byte guest encoding and register layout remain intact,
but arithmetic loses extended precision and range. Native sanitizer checks and
a Wasm/Node arithmetic smoke run pass.

`sm64ex-wasm` now partitions the final linked i386 ELF by linker-map object
ownership, lifts each code unit to its own bitcode file, optimizes each Wasm
object at `ELFCONV_SM64EX_WASM_OPT_LEVEL`, and links those units with cached shared metadata and runtime
objects. Its cache is under
`examples/sm64ex/build/us_pc_pie/.elfconv-incremental/`. Unchanged units skip
lifting and compilation; changed lifter inputs invalidate cached units. This
uses linker-resolved code, not raw relocatable `.o` files. VMA/layout changes
can invalidate additional units, and the final Wasm link remains whole-program
without cross-unit LTO.

Lifted i386 browser builds require WebGL2 (`MIN_WEBGL_VERSION=2` and
`MAX_WEBGL_VERSION=2`), including both incremental and whole-program conversion.
SM64EX uses Emscripten's SDL2/OpenGL bridge with legacy GL emulation disabled;
the browser build does not fall back to WebGL1. Native rendering is unchanged.

The generated HTML displays the latest frame's work time in milliseconds, not
FPS or the interval between frames. For SM64, timing starts at the first input
event poll and ends at the pacing-timer sample after rendering and audio.
Browser swap waits are subtracted, and the final frame-rate sleep is excluded.
This is CPU-side elapsed time, not a GPU timer. The display resets when the tab
changes visibility or the program exits/aborts.

The PIE browser build was exercised in Chromium and reaches the title screen,
with Mario, `PRESS START`, and the `SUPER MARIO 64` background visible. The earlier splash intentionally shows
only the `64` logo. The title background is still visibly tiled, and gameplay
beyond the title screen has not been verified; this is not yet a complete
playable-browser claim. Upstream's `TARGET_WEB` build compiles source directly
with Emscripten and is not ELF lifting.
