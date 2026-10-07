#!/bin/sh
set -eu
SYSROOT=${SYSROOT:-$HOME/.local/opt/elfconv-i386-sysroot}
CLANG=${CLANG:-$HOME/.local/opt/llvm-16.0.4/bin/clang}
exec "$CLANG" -m32 -fno-pie --sysroot="$SYSROOT" \
  -I"$SYSROOT/usr/include/i386-linux-gnu" \
  -B"$SYSROOT/usr/lib/i386-linux-gnu" \
  -Wno-unused-command-line-argument \
  -L"$SYSROOT/usr/lib/gcc/i686-linux-gnu/15" "$@"
