# Usage

## Overview

elfconv provides the following conversion targets:

| Target | `TARGET` value | Output | Runtime |
|---|---|---|---|
| Browser (Wasm) | `aarch64-wasm` | `.wasm` + `.js` + `.html` | Browser (Emscripten) |
| WASI (Wasm) | `aarch64-wasi32` | `.wasm` | WasmEdge etc. |
| Native | `aarch64-native` | Host ELF binary | Direct execution |
| Experimental i386 browser | `i386-wasm` | `.bc` + `.wasm` + `.js` | Emscripten host-import adapters |

## Build Commands

### Browser (ELF → Wasm)

`INITWASM=1` generates `js-kernel.js` and `main.html`. This should be set only for the **init (main) program**. When building additional binaries for multi-process use (e.g., busybox for use from bash), omit `INITWASM`.

**1. Single binary:**
```bash
cd build
TARGET=aarch64-wasm INITWASM=1 ../scripts/dev.sh /path/to/elf
```

**2. Multi-process example (bash + busybox):**
```bash
cd build
# Build the init program (bash) with INITWASM=1
TARGET=aarch64-wasm INITWASM=1 ../scripts/dev.sh /path/to/bash-static
# Build additional binaries without INITWASM
TARGET=aarch64-wasm ../scripts/dev.sh /path/to/busybox
```

**3. With host directory mounting:**
```bash
TARGET=aarch64-wasm INITWASM=1 MOUNT_SETTING="/host/dir@/mount/point" ../scripts/dev.sh /path/to/elf
```

### i386 SDL/OpenGL (ELF → Wasm)

The input must be a non-PIE i386 ELF executable. The supported libc/SDL2/OpenGL
imports resolve to guest-ABI adapters; Linux shared libraries and syscalls are
not emulated. This target does not use the AArch64 process-management JS kernel.

```bash
TARGET=i386-wasm ELFLIFT=/path/to/elflift EMCC=/path/to/em++ \
  ECV_OUT_DIR=/path/to/output bash bin/exe.sh /path/to/i386-elf
```

`ELFLIFT` defaults to `build/lifter/elflift`; `EMCC` defaults to `em++`.
The output directory must exist. The lifter must be built with x86 semantics.
For a complete browser page and the exercised Makefile workflow, see the
[SDL triangle example](../examples/README.md#sdl--opengl-2-triangle).
Unsupported imported functions fail at link time rather than receiving stubs.

### WASI (ELF → Wasm)
```bash
cd build
TARGET=aarch64-wasi32 ../scripts/dev.sh /path/to/elf
```

### Native (ELF → ELF)
```bash
cd build
TARGET=aarch64-native ../scripts/dev.sh /path/to/elf
```

## Environment Variables

| Variable | Description |
|---|---|
| `TARGET` | **Required.** Conversion target: `aarch64-native`, `aarch64-wasm`, `aarch64-wasi32`, or experimental `i386-wasm` |
| `INITWASM` | Set to `1` to generate `js-kernel.js` and `main.html` (browser target, init program only) |
| `NO_LIFTED` | Skip ELF → LLVM IR lifting (reuse existing `.bc`/`.ll` file) |
| `NO_COMPILED` | Skip LLVM IR → object file compilation (reuse existing `.o` file) |
| `MOUNT_SETTING` | Mount host directories into browser MEMFS. Format: `<host_dir>@<mount_point>` (multiple specs supported, space-separated) |
| `DEBUG` | Enable runtime debug macros (syscall debug, multi-section warnings) |
| `TEXTIR` | Output `.ll` (human-readable LLVM IR) instead of `.bc` |
| `FLOAT_STATUS` | Enable floating-point exception tracking |
| `ECV_OUT_DIR` | Override output directory (default: current directory) |
| `ELFLIFT` | Lifter executable path (default: `build/lifter/elflift`) |
| `EMCC` | Emscripten C++ compiler path (default: `em++`) |
