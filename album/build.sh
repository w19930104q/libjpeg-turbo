#!/bin/bash
# Build albumbatch against the locally-built static TurboJPEG library.
# Usage: ./album/build.sh [path-to-libjpeg-turbo-build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-local}"
CC_BIN="${CC:-/home/liiing/toolchain/gcc.sh}"

"$CC_BIN" -O2 -Wall -Wextra -Wno-unused-parameter \
  -I"$ROOT/src" -I"$BUILD" \
  "$ROOT/album/albumbatch.c" \
  "$BUILD/libturbojpeg.a" \
  -o "$ROOT/album/albumbatch"

echo "built $ROOT/album/albumbatch"
