#!/bin/bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [[ "${ELFCONV_BUILD:-0}" == 1 ]]; then
  cmake --build "$ROOT/build/llvm23" --target elflift \
    --parallel "${ELFCONV_BUILD_JOBS:-4}"
fi

STEAM="$HOME/snap/steam/common/.local/share/Steam"
PORTAL="$STEAM/steamapps/common/Portal"
RUNTIME="$STEAM/ubuntu12_32/steam-runtime"

LIBS=(
  "$PORTAL/bin/launcher.so"
  "$PORTAL/bin/libsteam_api.so"
  "$RUNTIME/usr/lib/i386-linux-gnu/libstdc++.so.6"
  "$RUNTIME/lib/i386-linux-gnu/libgcc_s.so.1"
  "$PORTAL/bin/libtier0.so"
  "$PORTAL/bin/libvstdlib.so"
  "$PORTAL/bin/libtogl.so"
  "$PORTAL/bin/engine.so"
  "$PORTAL/portal/bin/client.so"
  "$PORTAL/portal/bin/server.so"
  "$PORTAL/bin/filesystem_stdio.so"
)
ARGS=()
for library in "${LIBS[@]}"; do
  ARGS+=(--shared-library "$library")
done

python3 scripts/convert_elf.py \
  --input "$PORTAL/hl2_linux" \
  --target i386-wasm \
  --output-dir "$ROOT" \
  --wasm-opt-level "${ELFCONV_WASM_OPT_LEVEL:-1}" \
  "${ARGS[@]}" \
  --lifter "$PWD/build/llvm23/src/lifter/elflift" \
  --emcc "$HOME/emsdk-6/upstream/emscripten/em++" \
  --legacy-gl 0 \
  --max-ir-chunks-per-elf 100 \
  --runtime-archive "$PWD/build/llvm23/src/lifter/libelfconv-runtime-i386-wasm-incremental.a" \
  --jobs "${ELFCONV_JOBS:-16}" \
  "$@"

