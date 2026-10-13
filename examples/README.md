# Examples for conversion

Project-owned examples are configured from their directory or from the repository
root. Each SDL example exposes the same three target suffixes:

| Suffix | Build |
|---|---|
| `-i386` | Native Linux i386 SDL/OpenGL executable. |
| `-wasm` | Application sources compiled directly to browser Wasm. |
| `-i386-to-wasm` | Convert the i386 ELF through lifting, compiling, and linking. |

The nine primary targets are:

- `sdl_triangle-i386`, `sdl_triangle-wasm`, `sdl_triangle-i386-to-wasm`.
- `sdl_cube-i386`, `sdl_cube-wasm`, `sdl_cube-i386-to-wasm`.
- `sdl_cube_pthread-i386`, `sdl_cube_pthread-wasm`,
  `sdl_cube_pthread-i386-to-wasm`.

## Build

Native targets require an i386 C/C++ toolchain and i386 SDL2/OpenGL development
libraries. Direct Wasm targets require Emscripten 6.0.12; conversions also
require the repository's `elflift`.

The examples use native SDL/OpenGL on Linux and WebGL for browser rendering.
They do not include or select a software OpenGL renderer. Native drivers and
browsers may still provide their own software fallback.

Configure standalone or common-root targets:

```bash
cmake -S examples/sdl_triangle -B build/sdl_triangle \
  -DELFCONV_LIFTER="$PWD/build/src/lifter/elflift" \
  -DELFCONV_EMCC="$HOME/emsdk-6/upstream/emscripten/em++"
cmake --build build/sdl_triangle --target \
  sdl_triangle-i386 sdl_triangle-wasm sdl_triangle-i386-to-wasm

cmake -S . -B build -DELFCONV_ARCH=x86 \
  -DELFCONV_EMCC="$HOME/emsdk-6/upstream/emscripten/em++"
cmake --build build --target sdl_cube-i386-to-wasm
```

Cube conversions retain their linker maps and incremental caches. The common
build exposes all three example families through the same declarations.

## Outputs and browser serving

Artifacts are isolated below each example's CMake binary directory:
`artifacts/<name>/{i386,wasm,i386-to-wasm}/`. Each Wasm directory contains its
matching `.html`, `.js`, and `.wasm`; cube conversion caches and maps stay with
their matching variant.

Serve the generated HTML over HTTP. Pthread-cube Wasm requires COOP/COEP headers:

```sh
OUT="$PWD/build/sdl_cube_pthread/artifacts/sdl_cube_pthread/wasm"
TEST_PORT=8000 SERVE_DIR="$OUT" TEST_HTML="$OUT/sdl_cube_pthread-wasm.html" \
  node test/browser/test-server.js
```

The repository server sets `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: credentialless`. Converted pthread builds retain
the cube's second application thread, synchronization, and join behavior.

The SDL examples use ordinary SDL GL context and procedure APIs. Converted
builds resolve their graphics imports to browser WebGL adapters; context
creation failure is reported rather than replaced with a project software
renderer. This does not establish compatibility with arbitrary Linux binaries.
Unsupported relocations, imports, pointer-bearing SDL events, and raw i386
syscalls fail.

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

After configuring the repository build for `ELFCONV_ARCH=x86`, build any
combination of SM64EX variants from the repository root:

```bash
cmake -S . -B build \
  -DELFCONV_SM64EX_SYSROOT="$HOME/.local/opt/elfconv-i386-sysroot" \
  -DELFCONV_EMCC="$HOME/emsdk-6/upstream/emscripten/em++"
cmake --build build --target sm64ex-i386 sm64ex-wasm sm64ex-i386-to-wasm
```

`sm64ex-i386` builds the i386 PIE ELF; `sm64ex-wasm` compiles the source
directly to browser Wasm; `sm64ex-i386-to-wasm` converts the i386 ELF.
`sm64ex-bitcode` remains available as the intermediate lifting-only target.
Direct Emscripten output is isolated under
`examples/sm64ex/build/us_web_emscripten/`; native and converted outputs are
under `examples/sm64ex/build/us_pc_pie/`. The direct build needs Emscripten but
not the i386 sysroot or lifter; native needs the i386 toolchain, and conversion
additionally needs the lifter and Emscripten.

The native build requires i386 SDL2/OpenGL development libraries. CMake passes
the compiler through `CC` and sysroot flags through `PLATFORM_CFLAGS`.

The CMake targets invoke the pinned upstream Makefiles to build SM64EX's asset
tools and game. Conversion lifts the linked PIE ELF with a fixed `0x08000000`
guest load bias; this is AOT PIE support, not ASLR or a general shared-library
loader.

### Shared-library configuration

Enable `ELFCONV_SM64EX_SHARED` to build the main game as a PIC i386 `.so`
(`-fPIC -shared -Wl,-z,defs`) instead of a PIE executable:

```sh
cmake -S . -B build -DELFCONV_SM64EX_SHARED=ON
cmake --build build --target sm64ex-i386
examples/sm64ex/build/us_pc_shared/sm64-runner \
  "$PWD/examples/sm64ex/build/us_pc_shared/sm64.us.f3dex2e.so" --help
cmake --build build --target sm64ex-i386-to-wasm
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
program exit status. Standalone conversion does not automatically load Linux
dependencies; explicitly bundled libraries can use the guest dynamic loader below.
Symbol interposition and arbitrary host-facing export signatures remain unsupported.
`stdin`, `stdout`, and `stderr` data imports relocate to guest stream-handle
slots, including executable copy relocations; host `FILE*` pointers are not exposed.

### Bundled dynamic libraries

The Python driver accepts repeated `--shared-library` arguments:

```sh
python scripts/convert_elf.py \
  --input /absolute/path/to/program --target i386-wasm \
  --shared-library /absolute/path/libdependency.so \
  --shared-library /absolute/path/libplugin.so \
  --lifter "$PWD/build/src/lifter/elflift" \
  --emcc /path/to/emsdk/upstream/emscripten/em++ \
  --output-dir /tmp/program-wasm
```

The equivalent lifter flag is `--shared_libraries=/path/a.so,/path/b.so`;
`elfconv_add_conversion_target` accepts `SHARED_LIBRARIES` as a list.
All bundled code and relocated data are included in the generated Wasm.
Libraries occupy distinct guest ranges between `0x01000000` and `0x04000000`,
below the heap. `DT_NEEDED` references to bundled SONAMEs establish constructor
and symbol-lookup dependency order; unbundled system imports still need host adapters.
Allocated library data is restored on reload after the last reference closes.
Constructor/finalizer pointers are discovered even when their local symbols
were stripped from an otherwise partially symbolized ELF.
External function references through `R_386_32`, `R_386_PC32`, or `R_386_GLOB_DAT`
use guest-addressed adapter thunks when no bundled definition exists.
This includes address-taken libc functions and direct calls that bypass the PLT.
The named adapter must exist at Wasm link time; unknown functions still fail.

Guest `dlopen` matches supplied paths, SONAMEs, or filename aliases (including
relative paths such as `bin/launcher.so`). `RTLD_LAZY` and `RTLD_NOW` both use
AOT-resolved bindings. `RTLD_LOCAL`, `RTLD_GLOBAL`, and `RTLD_NOLOAD` are supported,
as is `dlopen(NULL, ...)` for main-program exports. `dlsym` returns callable guest
function addresses or guest data addresses, and searches a handle's dependency
closure. `RTLD_DEFAULT` searches live global objects and retains lookup
dependencies for the calling object. A zero-valued export is distinct from a
lookup error; `dlerror` consumes the pending error. Last-close finalizers run
before dependencies are released; remaining libraries finalize at normal
process termination. Cyclic reference graphs remain resident until termination.

#### i386 ELF TLS

`PT_TLS` templates and zero storage are initialized separately for each guest
CPU `State`. The closed-world bundle assigns every module a fixed Variant-II
static TLS offset, including libraries opened later. Local-exec, initial-exec,
general-dynamic, local-dynamic, and GNU2 `R_386_TLS_DESC` accesses are supported.
`dlsym` of a TLS export returns its address in the calling state's block.
Opening an unloaded module restores its TLS template in all active contexts
before constructors run; reopening an already-live module preserves its values.

TLS occupies `0x00100000`–`0x01000000`, separate from library images and heap.
The guest GS base points at an i386 TCB with a DTV and stack/pointer guards.
`__cxa_thread_atexit_impl`/`__cxa_thread_atexit` callbacks run LIFO at thread exit;
their owning libraries remain live until callbacks finish. Main-thread TLS
destructors run before global finalizers, with TLS storage valid for both.

`src/runtime/I386Tls.h` initializes and finalizes each guest pthread's TLS.
Host pthreads provide execution and synchronization; ELF module IDs, guest TLS
addresses, GS semantics, and TLS destructors remain guest-owned.

#### C++ runtime bundles

Supply compatible i386 `libstdc++.so.6` and `libgcc_s.so.1` through the same
`ELFCONV_SHARED_LIBRARIES` list. C++ vtables, RTTI objects, methods, and casts
then execute as guest code/data; no host C++ object or vtable pointers are used.
The `rtti` differential fixture bundles real implementations from the compiler's
`libsupc++.a` and verifies single/multiple/virtual inheritance and cross-library
casts. That isolated regression does not establish full libstdc++ compatibility.

For the installed Portal, the diagnostic bundle consists of `bin/launcher.so`,
Steam's `ubuntu12_32/steam-runtime/usr/lib/i386-linux-gnu/libstdc++.so.6`,
`ubuntu12_32/steam-runtime/lib/i386-linux-gnu/libgcc_s.so.1`, and Portal's
`bin/libtier0.so`, `bin/libvstdlib.so`, and `bin/libtogl.so`.
Use absolute paths for all six entries; system SDL/libc/pthread libraries still
require guest-ABI adapters rather than native host pointers.

Bundled i386 Wasm conversion lifts each ELF separately with bounded concurrent
workers, then links one Wasm module with shared guest layout/loader metadata and
import adapters. Portal uses this per-ELF path. Cache: output directory's
`.elfconv-incremental/`.
`--max-ir-chunks-per-elf N` caps chunks per ELF; `N=1` skips partitioning and
cloning. `N=0` disables chunk output (default). No bundle-wide allocation or relift.
`--jobs N` caps lifting and compiling jobs combined (default 2); each written IR
chunk enters the shared pool before its ELF finishes partitioning. Ready ELF owners rotate
through the pool. CMake: `ELFCONV_JOBS` (default 2); target argument: `JOBS`.
Shared metadata, registry, and runtime compilation share the pool; final link
waits for them and all chunks. Successful stages retain cache keys if another
job fails. TTY: live per-library progress tree. Redirected output: plain stage
transitions. Tool logs: `.elfconv-logs/`; failures print log tail and path.
`--analyze_only` reports ELF architecture, executable sections, function-extent
distribution, and largest extents without translating instructions.
`--lift_profile_out FILE.tsv` records per-function lift time, input extent, LLVM
instruction/block counts, and printed IR bytes.


This is an explicit AOT bundle, not a runtime native-ELF compiler or a general
Linux dynamic linker. `RTLD_NEXT`,
deep binding, NODELETE, IFUNC resolution, symbol versions/interposition, and
filesystem/LD_LIBRARY_PATH searches are not implemented. Unknown library names
fail rather than invoking host `dlopen` or exposing host pointers.

The installed Portal `hl2_linux` alone links and runs its missing-launcher error
path (`Failed to load the launcher`). The six-library C++ diagnostic bundle now
lifts 5,641 functions and resolves the previous TLS, RTTI, `stdin`, and direct
`getpid` relocation blockers. CPU initialization no longer reports missing
`FXSAVE` semantics. Wasm linking still fails on unimplemented adapters, including
pthread mutexes, SDL surface/window functions, and libc string/filesystem APIs.
Current XED memory `FADD`, `RDRAND`, and 28-byte x87 environment forms lift without
unsupported-instruction reports and pass native-versus-Wasm regressions.
Adding engine, client, server, and filesystem lifts 17,008 functions. Optional
AVX code still reports a decode failure; the virtual CPU advertises no AVX.
i386 uses a 512 MiB guest arena, with bundled libraries below the executable image.
Native-versus-Wasm regressions cover the added CPU, AES, packed SSE, and libc adapters.
This is not a successful Portal game run.



### Runtime and incremental conversion

The native executable's `--help` command succeeds, and the lifted bitcode
passes LLVM verification. Lifting required fixes for `stdout` copy relocations,
variable-length x86 instruction scanning in indirect-jump functions, i386
indirect-branch PHI widths, and missing SIMD/NOP semantics.

x87 arithmetic now uses binary64 `double`, including math builtins and NaN
classification. The ten-byte guest encoding and register layout remain intact,
but arithmetic loses extended precision and range. Native sanitizer checks and
a Wasm/Node arithmetic smoke run pass.

`sm64ex-i386-to-wasm` partitions the final linked i386 ELF by linker-map object
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
