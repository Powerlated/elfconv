# How to use release packages

Release Page: https://github.com/yomaytk/elfconv/releases

## Quick Start

### Prerequisites

- CMake 3.21+
- Emscripten for browser targets
- WASI-SDK and a WASI runtime for WASI targets

### Browser conversion

```bash
tar -xzf elfconv-<VERSION>-linux-<arch>.tar.gz
cd outdir
cmake \
  -DELFCONV_INPUT=/absolute/path/to/elf \
  -DELFCONV_TARGET=aarch64-wasm \
  -DELFCONV_OUTPUT_DIR="$PWD/out" \
  -DELFCONV_LIFTER="$PWD/bin/elflift" \
  -DELFCONV_EMCC=/path/to/emsdk/upstream/emscripten/em++ \
  -DELFCONV_INIT_WASM=ON \
  -P cmake/ConvertElf.cmake
```

To add binaries to the browser page, run conversion again with the same output
directory and omit `ELFCONV_INIT_WASM`. To preload a host directory, set
`ELFCONV_MOUNT_SETTINGS=/host/dir@/mount/point`.

For WASI, set `ELFCONV_TARGET=aarch64-wasi32` and
`ELFCONV_WASI_SDK=/path/to/wasi-sdk`; the output is in `out/`.
