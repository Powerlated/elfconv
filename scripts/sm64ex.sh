#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
GAME="$ROOT/examples/sm64ex"
export SYSROOT=${SYSROOT:-$HOME/.local/opt/elfconv-i386-sysroot}
export CLANG=${CLANG:-$HOME/.local/opt/llvm-16.0.4/bin/clang}
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/i386-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export LD_LIBRARY_PATH="$HOME/.local/opt/dwarf-2021/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export ELFLIFT=${ELFLIFT:-$ROOT/build-llvm16.0.4/lifter/elflift}
export EMCC=${EMCC:-$HOME/emsdk/upstream/emscripten/em++}
MODE=${1:-wasm}
case "$MODE" in native|bitcode|wasm) ;; *) echo "Usage: $0 [native|bitcode|wasm]" >&2; exit 2 ;; esac
[[ -f "$GAME/baserom.us.z64" ]] || { echo "Missing $GAME/baserom.us.z64" >&2; exit 1; }
# Asset extractors run on the host, not in the i386 guest sysroot.
make -C "$GAME/tools/audiofile" CC=gcc CXX=g++
make -C "$GAME/tools" CC=gcc CXX=g++
make -C "$GAME" -j"${JOBS:-8}" TARGET_BITS=32 TARGET_ARCH=i686 NO_PIE=1 \
  CC="$ROOT/scripts/sm64ex-cc.sh" 'SDLCONFIG=pkg-config sdl2'
ELF="$GAME/build/us_pc/sm64.us.f3dex2e"
[[ "$MODE" != native ]] || exit 0
if [[ "$MODE" == bitcode ]]; then
  "$ELFLIFT" --arch i386 --target_arch emscripten32 --target_elf "$ELF" \
    --bc_out "$ELF.bc" --norm_mode 1 --fork_emulation 0 --float_exception 0
else
  TARGET=i386-wasm ECV_LEGACY_GL=0 ECV_OUT_DIR="$GAME/build/us_pc" \
    bash "$ROOT/bin/exe.sh" "$ELF"
fi
