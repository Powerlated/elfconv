
<p align="center">
  <img src="images/elfconv-official.png" width="300" alt="elfconv-logo2" />
</p>


# elfconv
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/yomaytk/elfconv)

An ahead-of-time binary translator that converts Linux applications to WebAssembly for both browser and WASI runtimes.

[[🌎**Try Demo**]](https://yomaytk.github.io/elfconv-demo/)

## Why elfconv?
### 1. Easy Porting to Wasm
- **No source code needed** — Port pre-built binaries directly to Wasm, even when source code is unavailable
- **Linux syscall emulation** — Emulates Linux syscalls missing in Wasm environments (e.g., `fork/execve`), enabling unmodified Linux apps to run
- **Language-agnostic** — No per-language compiler support needed; works with any language that compiles to Linux/ELF

### 2. Process Management
- **`fork/execve` support**
  - elfconv experimentally supports `fork()` and `execve()` system calls in Wasm in browsers. This enables running more complex Linux applications that rely on multi-process architectures, such as shells, build systems, and server applications that spawn child processes (e.g., [**GNU Bash + BusyBox**](https://yomaytk.github.io/elfconv-demo/bash-busybox.html) on the demo page).

### 3. Low Performance Overhead
- Existing projects use CPU emulators (e.g., [v86](https://github.com/copy/v86), [container2wasm](https://github.com/container2wasm/container2wasm)) to port Linux applications to Wasm, but incur severe slowdowns of **10× or more**. In contrast, elfconv applies AOT compilation to translate machine code directly to LLVM IR, achieving much smaller performance degradation (ref.  [Performance](#performance) section).

## Current Limitations
> [!WARNING]
> **elfconv is WORK IN PROGRESS**. You may encounter failures when compiling ELF binaries or executing generated Wasm binaries.

- **Architecture Support**: AArch64 ELF binaries are supported
  - x86-64 support is under development.
  - Experimental i386 support converts the dynamically linked, non-PIE
    [SDL/OpenGL triangle](examples/README.md#sdl--opengl-2-triangle) and
    [shader cube](examples/README.md#sdl--opengl-cube) to Wasm. The triangle's
    framebuffer readback and interactive keyboard exit pass; the cube's
    90-frame readback and clean exit pass. This does not establish
    compatibility with arbitrary i386 binaries or Portal.
  - x87 arithmetic uses 64-bit `double`, not extended 80-bit precision.
    The ten-byte guest encoding and register layout are retained, but values
    are rounded to binary64 for arithmetic. Extended precision and range are lost.
    i386 `FXSAVE`/`FXRSTOR` preserve stored x87 encodings, control/status/tags,
    MXCSR, and XMM state. MMX/x87 alias tracking remains incomplete.
- **Linking**: No general Linux shared-object loader
  - AArch64 conversion requires statically linked binaries.
  - `i386-wasm` resolves the documented samples' libc/SDL2/OpenGL imports to
    Emscripten guest-ABI adapters. It does not load the corresponding Linux
    libraries. i386 PIE executables use a fixed guest load bias of `0x08000000`
    (no ASLR), with relative relocations and GOT/PLT import resolution.
    Standalone i386 shared libraries can select an exported `int(int,char**)`
    entry with `ELFCONV_ENTRY_SYMBOL` (lifter: `--entry_symbol`). Constructors
    run before the entry and finalizers after it returns. i386
    conversion can bundle additional ELF libraries with `ELFCONV_SHARED_LIBRARIES`
    (lifter: `--shared_libraries`) for guest `dlopen`/`dlsym`/`dlclose`/`dlerror`.
    Symbols remain guest addresses; dependency constructors and library
    reference counts are honored. Libraries must be supplied at conversion time.
    Defined symbols bind within their ELF, with unresolved references resolved
    against the bundled set or existing host adapters. Selected stdio imports
    include `clearerr`, `freopen`, `freopen64`, `ftello`, `getchar`, `gets`,
    `putc_unlocked`, `swprintf`, and `vprintf`. Legacy `gets` has no object-size
    bound; writes are checked only against the guest arena.
    C23 `vfscanf`/`vsscanf` support binary `%b`; `fa_IR` string scans support
    Persian digits with `%I`; scanf `%d` overflow saturates and partial-width
    `%c` returns EOF. GNU printf `%m`
    and `fopen("/dev/zero")` are handled. `mtrace` is supported only when
    `MALLOC_TRACE` is unset or empty. Selected POSIX/GNU helpers include
    `fstat`, `fstat64`, `ftruncate`, `mkdtemp`, `pathconf`, and
    `strerrorname_np`; `mprotect` tracks permissions, but Wasm cannot enforce
    protected-page faults. General Linux loader search/interposition,
    unsupported relocations, and unknown imports remain unsupported. i386 ELF
    TLS supports local/initial-exec, general/local-dynamic, and GNU2 descriptors,
    with per-CPU-state storage and thread-local destructors.
    `ELFCONV_PTHREADS=ON` enables guest create/join, mutexes, and condition
    variables on Emscripten workers; requires JSPI and cross-origin isolation.
    See [bundled-library usage](examples/README.md#bundled-dynamic-libraries).
    Raw i386 syscalls are rejected explicitly by the runtime.
- **System Calls**: Partial Linux syscall implementation
  - See [`src/runtime/syscalls/`](https://github.com/yomaytk/elfconv/blob/main/src/runtime/syscalls) for currently supported syscalls

## Portal API surfaces

This inventory covers the API families identified for the documented Portal ELF
bundle; exact imports vary by launch path. No complete Portal game run has passed.
Vendored test status uses `vendor: passing/included/total vendored tests`.

| Surface | Portal needs | elfconv status | Vendored test status |
|---|---|---|---|
| i386 ABI and CPU | 32-bit calling conventions and instructions used by the game and bundled libraries | SDL/OpenGL samples pass. Portal bundle lifts 17,008 functions; optional AVX code fails to decode and x87 arithmetic uses binary64. | QEMU i386: 0/2/2 |
| ELF startup and shared objects | `hl2_linux`, `launcher.so`, `libtier0.so`, `libvstdlib.so`, `libtogl.so`, Steam's `libstdc++.so.6` and `libgcc_s.so.1`; expanded bundle also includes engine, client, server, and filesystem ELFs | Explicit bundling supports guest constructors, TLS, and dynamic symbol lookup; no general loader search/interposition or IFUNC support. `hl2_linux` alone reaches only the missing-launcher error path. | glibc loader: 0/1/1 |
| C and C++ runtimes | glibc memory/string, stdio, parsing, math, locale, plus C++ ABI, RTTI, exceptions, and unwind behavior | Partial libc adapters and a project-owned RTTI regression exist. Portal linking still reports unresolved libc string/filesystem imports; full runtime compatibility is unverified. | glibc libc: 0/1/1<br>glibc stdio-common: 6/188/476 |
| POSIX and Linux services | Game-data filesystem, process/environment, mapping, clocks, sockets, and required syscalls | Selected guest-ABI adapters exist; raw i386 Linux syscalls are rejected and the Portal import set is unresolved. | glibc filesystem: 0/1/1 |
| pthreads and ELF TLS | Worker creation, synchronization, thread-specific data, destructors, and library TLS | Adapters and ELF TLS models exist; Portal thread behavior is unverified. Browser threads require JSPI and cross-origin isolation. | glibc pthread: 0/2/2 |
| SDL2 window, input, and surfaces | Window/context lifecycle, events, input, surfaces, and pixel operations | Triangle/cube exercise limited paths. SDL surface/pixel lifted conversion currently fails; broader SDL compatibility is unproven. | SDL2: 0/17/17 |
| OpenGL | Context setup, GL entry-point lookup, rendering, and readback through `libtogl.so` | Sample cube rendering/readback passes; Portal's `libtogl` call set is unverified. | |
| Steamworks | Steam startup, client/user/utilities, stats, and callbacks where used by the launch path | A project-owned fixture covers selected interfaces; Portal compatibility is unverified. | |
| Audio | Usable audio provider/device output for gameplay | Portal audio has not been exercised; the Steam API test notes a real audio provider prerequisite. | |

See [i386 coverage notes](test/elfconv/README.md), the
[SDL2 coverage map](test/vendored/sdl2/README), and
[bundled-library diagnostics](examples/README.md#bundled-dynamic-libraries).

## Performance
We measured the performance of Wasm generated by elfconv against Wasm compiled directly from source using Emscripten or WASI-SDK. The workflow is:
1. Compile source code → AArch64 ELF binary (using gcc/clang)
2. Convert ELF binary → Wasm using elfconv
3. Compare with: Source code → Wasm directly (using Emscripten/WASI-SDK)

**Blog**: [elfconv: Linux Apps to High-Performance Wasm Binary Translator](https://medium.com/nttlabs/some-performance-improvements-in-the-binary-translator-that-converts-linux-applications-c6b26945cf39)
### To Wasm (Emscripten)
| Target Program      | from source code   | elfconv    |
|----------|----------|----------|
| [`eratosthenes_sieve`](https://github.com/yomaytk/elfconv/tree/main/examples/benchmarks/eratosthenes_sieve) (↓ better) | 0.567 (s) (100%) | 0.692 (s) (**82%**)  |
| [`LINPACK benchmark`](https://www.netlib.org/linpack/) (↑ better) | 1617 (MFLOPS) (100%) | 1256 (MFLOPS) (**78%**) |
| [`mnist-neural-network-plain-c (during 30 steps)`](https://github.com/AndrewCarterUK/mnist-neural-network-plain-c) (↑ better) | 2.138 (s) (100%) | 2.255 (s) (**96%**) |

### To WASI
| Target Program      | from source code   | elfconv    |
|----------|----------|----------|
| [`eratosthenes_sieve`](https://github.com/yomaytk/elfconv/tree/main/examples/benchmarks/eratosthenes_sieve) (↓ better) | 0.362 (s) (100%) | 0.608 (s) (**60%**)  |
| [`LINPACK benchmark`](https://www.netlib.org/linpack/) (↑ better) | 4821 (MFLOPS) (100%) | 2720 (MFLOPS) (**56%**) |
| [`mnist-neural-network-plain-c (during 30 steps)`](https://github.com/AndrewCarterUK/mnist-neural-network-plain-c) (↑ better) | 2.271 (s) (100%) | 2.302 (s) (**96%**) |

### SM64EX bitcode size

Clean i386 non-PIE build (`us_pc/`, Clang 16.0.4, `-O2 -fno-pie -no-pie`),
measured 2026-10-09. Monolithic `examples/sm64ex/build/us_pc/sm64.us.f3dex2e.bc`,
before downstream LLVM optimization (`--arch i386 --target_arch emscripten32 --norm_mode 1`).
All measurements use sm64ex `d7ca2c04364a6dd0dac58b47151e04e26887e6f0`.

| Pass | Lifter LLVM | elfconv commit | Bytes | MiB | Change from baseline |
|---|---|---|---:|---:|---:|
| Baseline | 16.0.4 | `0129ba2e61b3828c8676c789927029efa9c310b5` | 33,814,388 | 32.25 | — |
| Local dead EFLAGS | 16.0.4 | `5c82faf657d55985109da82ee61292924d009ece` | 33,815,496 | 32.25 | +1,108 bytes |
| Block EFLAGS liveness | 23.1.2 | `b331cef7ecf21e8a5fb4f0fc003c850d98049cf0` | 32,502,028 | 31.00 | −1,312,360 bytes (−3.88%) |

Block liveness tracks individual arithmetic flags across safe register-only
instructions, stopping at memory, control flow, and uncertain effects.
Dead flag stores and their unused computations are removed from semantic variants
before bitcode emission. The latest size change includes the LLVM upgrade;
it does not isolate EFLAGS savings.

`-feflags-elision` enables arithmetic flag liveness and dead computation removal
together (default). `-fno-eflags-elision` preserves all flag computations and skips
the analysis. The last occurrence wins; there is no analysis-only option.


## Quick Start

### Build

Requirements: CMake 3.21+, Ninja, LLVM 23+, Python 3.11+, and Emscripten 6 for browser builds.
Verified toolchains: LLVM 23.1.2 and Emscripten 6.0.12 (bundled LLVM 24).
Set `LLVM_ROOT` to the LLVM installation used by Remill. The dependency superbuild
fetches and installs gflags, glog, GoogleTest, and XED.
The lifter expects the libdwarf 20210528 API; libdwarf 0.11.1 is incompatible.
BFD, libelf, and Zstandard development headers/libraries are also required.
Conversion uses Rich. Install into a virtual environment before configuring:

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r scripts/requirements.txt
```

```bash
export LLVM_ROOT=/path/to/llvm
cmake -S dependencies -B build/dependencies -G Ninja \
  -DUSE_EXTERNAL_LLVM=ON \
  -DLLVM_DIR="$LLVM_ROOT/lib/cmake/llvm" \
  -DCMAKE_PREFIX_PATH="$LLVM_ROOT" \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/dependencies/install" \
  -DCMAKE_C_COMPILER="$LLVM_ROOT/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_ROOT/bin/clang++"
cmake --build build/dependencies

cmake -S . -B build -G Ninja \
  -DELFCONV_ARCH=x86 \
  -DLLVM_DIR="$LLVM_ROOT/lib/cmake/llvm" \
  -DCMAKE_PREFIX_PATH="$PWD/build/dependencies/install;$LLVM_ROOT" \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install" \
  -DCMAKE_C_COMPILER="$LLVM_ROOT/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_ROOT/bin/clang++"
cmake --build build --target elflift
cmake --install build
```

Set `ELFCONV_ARCH=aarch64` when building the AArch64 semantics instead. Configure
the dependency and project builds with the same LLVM installation.
After rebuilding `elflift`, rerun conversion. Incremental caches automatically
invalidate when the lifter binary changes.
Build with `-DCMAKE_BUILD_TYPE=Release` for optimized lifting.
For `i386-wasm`, runtime sources compile as independent cached objects.
`--max-ir-chunks-per-elf N` caps chunks independently for each ELF.
`N=1` writes the original module without partitioning or cloning; `N=0`
disables chunk output (default). Larger limits are capped by function count.
LLVM keeps dependent definitions together; chunk sizes can differ.
Chunked conversion cannot use linker-map incremental units. `--jobs N` bounds
lifting and compiling jobs combined (default 2). Each written IR chunk enters
the shared pool before its ELF finishes partitioning; ready owners rotate through the pool.
CMake uses `ELFCONV_JOBS` (default 2); target argument: `JOBS`.
Progress rows show ELF, stage detail, counter, then stage time.
Compiling counters count completed chunks, not chunk identities.
Chunk output separates module verification, bitcode serialization, and bitcode writing.
Writing shows `[N bytes]` flushed to temporary output.

Portal: `./lift_portal.sh` reuses the repository-root cache and uses a shared
lifting/compiling pool with `-O1`. Overrides: `ELFCONV_JOBS`,
`ELFCONV_WASM_OPT_LEVEL`; extra driver flags pass through.
After lifter/runtime edits, run `ELFCONV_BUILD=1 ./lift_portal.sh` to rebuild first.
Lifting shows `[completed/known total]`; the total grows as callees are discovered.
After function translation, the display names IR finalization, partitioning,
bitcode writing, and cleanup instead of retaining a completed function counter.

`elflift --analyze_only` reports ELF headers, executable section sizes, and
function-extent estimates without lifting; add `--per_elf` to group a bundled
i386 ELF and its shared libraries. Initializer callbacks without symbol sizes
can have broad estimated extents. For per-function lift time and LLVM IR metrics
during a normal lift, use `--lift_profile_out=/path/profile.tsv`.
Direct CLI: `python scripts/convert_elf.py --help`. CMake conversion targets
invoke the same driver. Select the virtual environment with
`-DPython3_EXECUTABLE="$PWD/.venv/bin/python"` if needed.

### Browser

Activate Emscripten 6.0.12 with `emsdk install 6.0.12`, `emsdk activate 6.0.12`,
then source that SDK's `emsdk_env.sh`. Rebuild semantics and converted artifacts
after changing LLVM or Emscripten; do not reuse LLVM 16 bitcode.

Configure the project with the ELF input and conversion target, then build
`convert-elf`:

```bash
cmake -S . -B build \
  -DELFCONV_INPUT=/absolute/path/to/program \
  -DELFCONV_CONVERT_TARGET=aarch64-wasm \
  -DELFCONV_INIT_WASM=ON \
  -DELFCONV_EMCC="$EMSDK/upstream/emscripten/em++"
cmake --build build --target convert-elf
python3 -m http.server 8080 --bind 127.0.0.1 --directory build
```

Open `http://127.0.0.1:8080/main.html`. The lifter must be built for the
input ELF architecture (`ELFCONV_ARCH=x86` or `aarch64`). Additional input
files can be mounted with `-DELFCONV_MOUNT_SETTINGS=/host/path@/mount/path`.

### WASI Runtime

Configure `ELFCONV_CONVERT_TARGET=aarch64-wasi32` and set
`ELFCONV_WASI_SDK` to a WASI SDK directory. Build `convert-elf` as above, then
run the generated `.wasm` with WasmEdge or another WASI runtime:

```bash
cmake -S . -B build \
  -DELFCONV_INPUT=/absolute/path/to/program \
  -DELFCONV_CONVERT_TARGET=aarch64-wasi32 \
  -DELFCONV_WASI_SDK="$WASI_SDK_PATH"
cmake --build build --target convert-elf
wasmedge build/program.wasm
```

## Contributing

We'd love your help making it better!

### Examples
- **Instruction Support** — Implement missing AArch64 instructions or advance x86-64 instruction support.
- **System Call Implementation** — Add Linux syscall support in [`src/runtime/syscalls/`](https://github.com/yomaytk/elfconv/tree/main/src/runtime/syscalls)
- **Testing** — Test with various Linux applications and report issues

For questions or discussions, feel free to [open an issue](https://github.com/yomaytk/elfconv/issues/new) or start a discussion.

## Acknowledgement
elfconv uses or references some projects as follows. Great thanks to all its developers!
- Remill ([Apache Lisence 2.0](https://github.com/lifting-bits/remill/blob/master/LICENSE))
    - Original Source: https://github.com/lifting-bits/remill
    - elfconv uses Remill to convert machine codes to LLVM IR instructions. The source code is contained in [`./backend/remill`](https://github.com/yomaytk/elfconv/tree/main/backend/remill) and is modified for using from front-end and supporting additional instructions.
- [`src/browser/js-kernel.js`](https://github.com/yomaytk/elfconv/blob/main/src/browser/js-kernel.js) is derived from code generated by Emscripten (MIT licensed).
- MyAOT ([Apache Lisence 2.0](https://github.com/AkihiroSuda/myaot/blob/master/LICENSE))
    - Original Source: https://github.com/AkihiroSuda/myaot
    - An experimental AOT-ish compiler (Linux/riscv32 ELF → Linux/x86_64 ELF, Mach-O, Wasm, ...)
    - elfconv has started as the successor project of MyAOT.
