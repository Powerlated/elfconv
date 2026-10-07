# Examples for conversion

This directory has some example programs you can try converting the Linux/ELF. You can build the Linux/ELF by using a Makefile included in the every directory.

## SDL / OpenGL 2 triangle

`sdl_triangle` builds a dynamically linked, non-PIE i386 ELF executable. It
requests an OpenGL 2.0 context and draws an RGB triangle using compatibility
calls. Building the ELF requires an i386 C toolchain and i386 SDL2/OpenGL
development libraries.

```bash
cd examples/sdl_triangle
make
./triangle
make smoke
```

Escape or closing the window exits interactive mode. `make smoke` renders three
frames and checks that the triangle's center is colored and the background is
black. Override `CC`, `ARCH_FLAGS`, `PKG_CONFIG`, `LDFLAGS`, and `RUNNER` for a
local cross-toolchain or sysroot. Keep `-m32 -fno-pie -no-pie` when preparing
the ELF for conversion.

### Convert the ELF to Wasm

Build the x86-enabled elfconv lifter and install/configure Emscripten first.
Then, from `examples/sdl_triangle`:

```bash
make wasm ELFLIFT=../../build/lifter/elflift EMXX=em++
python3 -m http.server 8000 --bind 127.0.0.1
```

Open `http://127.0.0.1:8000/` for interactive rendering, or
`http://127.0.0.1:8000/?frames=3` for framebuffer checks and an exit status.
`make wasm` lifts `triangle` into `triangle.bc` and links it with the host-import
runtime to produce `triangle.js` and `triangle.wasm`. It does **not** compile
`triangle.c` with Emscripten. `make clean` removes these generated outputs.

The converted browser smoke has passed with center RGB `63,64,128`, a black
background, exit status 0, and interactive Escape handling. SDL2 is supplied
by Emscripten; legacy OpenGL calls use its immediate-mode emulation. The runtime
adapts i386 stack arguments, opaque SDL handles, returned strings, varargs, and
WebGL RGBA readback into desktop RGB/pack layout. Asyncify yields at buffer swaps
so rendering and input remain responsive.

This path handles the triangle's imported functions and relocations, not
arbitrary Linux shared libraries or retail Portal. PIE, unsupported relocations,
unknown host imports, unsupported pointer-bearing SDL events, and raw Linux
i386 syscalls fail explicitly. The printf adapter accepts scalar `%s`, `%d`,
`%u`, `%c`, `%x`, `%X`, and `%%` conversions; floating-point/length-modified
varargs are not implemented. Guest readback supports RGB/RGBA unsigned bytes.

For the isolated toolchains installed in this workspace, build from scratch with:

```bash
SYSROOT=\"$HOME/.local/opt/elfconv-i386-sysroot\"
export PKG_CONFIG_SYSROOT_DIR=\"$SYSROOT\"
export PKG_CONFIG_LIBDIR=\"$SYSROOT/usr/lib/i386-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig\"
export LD_LIBRARY_PATH=\"$HOME/.local/opt/dwarf-2021/usr/lib/x86_64-linux-gnu\"
make wasm \\
  CC=\"$HOME/.local/opt/llvm-16.0.4/bin/clang\" \\
  ARCH_FLAGS=\"-m32 -fno-pie -no-pie --sysroot=$SYSROOT -I$SYSROOT/usr/include/i386-linux-gnu -B$SYSROOT/usr/lib/i386-linux-gnu\" \\
  LDFLAGS=\"-L$SYSROOT/usr/lib/gcc/i686-linux-gnu/15\" \\
  ELFLIFT=../../build-llvm16.0.4/lifter/elflift \\
  EMXX=\"$HOME/emsdk/upstream/emscripten/em++\"
```

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

With the LLVM 16, i386 sysroot, and Emscripten toolchains described above:

```bash
bash scripts/sm64ex.sh native
bash scripts/sm64ex.sh bitcode
bash scripts/sm64ex.sh wasm
```

Run these commands from the repository root. Host asset tools are built with
GCC before the game is compiled as a non-PIE i386 ELF. Outputs are under
`examples/sm64ex/build/us_pc/`.

The native executable's `--help` command succeeds, and the lifted bitcode
passes LLVM verification. Lifting required fixes for `stdout` copy relocations,
variable-length x86 instruction scanning in indirect-jump functions, i386
indirect-branch PHI widths, and missing SIMD/NOP semantics.

Wasm generation is blocked by Emscripten's LLVM backend: compiling the lifted
x87 floating-point operations fails with `do not know how to soften fp_extend`
and `unsupported library call operation`. No working Wasm game or browser
runtime is verified. Upstream's `TARGET_WEB` build compiles source directly
with Emscripten and is not ELF lifting.
