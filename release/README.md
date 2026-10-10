# How to use release packages

Release Page: https://github.com/yomaytk/elfconv/releases

## Quick Start

### Prerequisites

- Python 3.11+; `python -m pip install -r scripts/requirements.txt` in a virtual environment
- Emscripten for browser targets
- WASI-SDK and a WASI runtime for WASI targets

### Browser conversion

```bash
tar -xzf elfconv-<VERSION>-linux-<arch>.tar.gz
cd outdir
python scripts/convert_elf.py \
  --input /absolute/path/to/elf --target aarch64-wasm \
  --output-dir "$PWD/out" \
  --lifter "$PWD/bin/elflift" \
  --emcc /path/to/emsdk/upstream/emscripten/em++ \
  --init-wasm
```

To add binaries to the browser page, run conversion again with the same output
directory and omit `--init-wasm`. Preload: `--mount /host/dir@/mount/point`.

WASI: `--target aarch64-wasi32 --wasi-sdk /path/to/wasi-sdk`.
