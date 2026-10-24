#!/bin/sh
# Builds LocalSend for Mac OS 9 with the Retro68 cross-compiler (see README.md). RETRO68 defaults to ~/Retro68-toolchain.
set -e
RETRO68="${RETRO68:-$HOME/Retro68-toolchain}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build/carbon"
cd "$ROOT/build/carbon"
cmake "$ROOT/os9" -DCMAKE_TOOLCHAIN_FILE="$RETRO68/powerpc-apple-macos/cmake/retrocarbon.toolchain.cmake" >/dev/null
make
echo "Built $ROOT/build/carbon/LocalSend.bin (MacBinary), LocalSend.dsk (disk image) and LocalSend.APPL"
