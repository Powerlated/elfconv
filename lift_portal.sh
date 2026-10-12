#!/bin/bash
STEAM="$HOME/snap/steam/common/.local/share/Steam"
PORTAL="$STEAM/steamapps/common/Portal"
RUNTIME="$STEAM/ubuntu12_32/steam-runtime"

LIBS=(
  "$PORTAL/bin/launcher.so"
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
  "${ARGS[@]}" \
  --lifter "$PWD/build/llvm23/src/lifter/elflift" \
  --emcc "$HOME/emsdk-6/upstream/emscripten/em++" \
  --legacy-gl 0 \
  --ir-chunks 96 \
  --runtime-archive "$PWD/build/llvm23/src/lifter/libelfconv-runtime-i386-wasm-incremental.a" \
  --lift-jobs 12 \
  --compile-jobs 12

