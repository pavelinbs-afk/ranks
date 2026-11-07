#!/bin/bash
# Build lr_core.so (TAB-only) for Linux inside the lvlranks-builder2 docker image.
# Usage (from host):
#   docker run --rm -v <repo-root>:/work lvlranks-builder2 bash -lc \
#     "sed -i 's/\r$//' /work/lr_core/build_linux.sh && bash /work/lr_core/build_linux.sh"
set -e

ROOT=/work
SRC=$ROOT/lr_core
BUILD=$SRC/build_linux

mkdir -p $BUILD
cd $BUILD

if command -v clang-10 >/dev/null 2>&1; then
  CC_BIN=clang-10
  CXX_BIN=clang++-10
else
  CC_BIN=clang
  CXX_BIN=clang++
fi

if [ ! -f .ambuild2/graph ] && [ ! -d .ambuild2 ]; then
  CC=$CC_BIN CXX=$CXX_BIN python3 $SRC/configure.py \
    --enable-optimize \
    --sdks=cs2 \
    --targets=x86_64 \
    --hl2sdk-root=$ROOT/deps \
    --hl2sdk-manifests=$ROOT/deps/hl2sdk-manifests \
    --mms_path=$ROOT/deps/metamod-source
fi

ambuild
echo "=== BUILD OK ==="
find $BUILD/package -type f | sed "s|$BUILD/||"
