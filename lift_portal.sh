#!/bin/bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

BUILD_TARGETS=(elfconv-runtime-i386-wasm-incremental-pthreads-legacygl)
if [[ "${ELFCONV_BUILD:-0}" == 1 ]]; then
  BUILD_TARGETS+=(elflift)
fi
cmake --build "$ROOT/build/" --target "${BUILD_TARGETS[@]}" \
  --parallel "${ELFCONV_BUILD_JOBS:-4}"

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
  "$PORTAL/bin/inputsystem.so"
  "$PORTAL/bin/materialsystem.so"
  "$PORTAL/bin/datacache.so"
  "$PORTAL/bin/studiorender.so"
  "$PORTAL/bin/vphysics.so"
  "$PORTAL/bin/video_services.so"
  "$RUNTIME/lib/i386-linux-gnu/libz.so.1"
  "$RUNTIME/lib/i386-linux-gnu/libexpat.so.1"
  "$RUNTIME/usr/lib/i386-linux-gnu/libfreetype.so.6"
  "$RUNTIME/usr/lib/i386-linux-gnu/libfontconfig.so.1"
  "$PORTAL/bin/vguimatsurface.so"
  "$PORTAL/bin/vgui2.so"
  "$PORTAL/bin/shaderapidx9.so"
  "$PORTAL/bin/stdshader_dx9.so"
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
  --lifter "$PWD/build/src/lifter/elflift" \
  --emcc "$HOME/emsdk-6/upstream/emscripten/em++" \
  --pthreads \
  --pthread-pool-size 8 \
  --legacy-gl 1 \
  --jspi \
  --max-ir-chunks-per-elf 100 \
  --gl4es-library "$ROOT/build/src/lifter/_deps/gl4es-src/lib/libGL.a" \
  --gl4es-include-dir "$ROOT/build/src/lifter/_deps/gl4es-src/include" \
  --runtime-archive "$PWD/build/src/lifter/libelfconv-runtime-i386-wasm-incremental-pthreads-legacygl.a" \
  --jobs "${ELFCONV_JOBS:-16}" \
  "$@"

