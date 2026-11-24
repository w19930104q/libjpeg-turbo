#!/bin/bash
# Build thumbrestart against the locally-built static TurboJPEG library.
# Usage: ./thumbrestart/build.sh [path-to-libjpeg-turbo-build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-local}"
CC_BIN="${CC:-/home/liiing/toolchain/gcc.sh}"

"$CC_BIN" -O2 -Wall -Wextra -Wno-unused-parameter \
  -I"$ROOT/src" -I"$BUILD" \
  "$ROOT/thumbrestart/thumbrestart.c" \
  "$BUILD/libturbojpeg.a" \
  "$BUILD/libjpeg.a" \
  -o "$ROOT/thumbrestart/thumbrestart"

echo "built $ROOT/thumbrestart/thumbrestart"
