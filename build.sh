#!/usr/bin/env bash
# Cross-compiles Velyx for Windows x64 from Linux using mingw-w64.
# See docs/BUILDING.md.

set -euo pipefail

cd "$(dirname "$0")"

BUILD_TYPE="Release"
CONSOLE="OFF"
BUILD_DIR="build"

for arg in "$@"; do
    case "$arg" in
        debug)   BUILD_TYPE="Debug"; CONSOLE="ON" ;;
        release) BUILD_TYPE="Release" ;;
        clean)   rm -rf "$BUILD_DIR" ;;
        *)       echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

if ! command -v x86_64-w64-mingw32-g++ >/dev/null; then
    echo "mingw-w64 is missing. On Debian/Ubuntu:" >&2
    echo "  sudo apt install mingw-w64 cmake ninja-build" >&2
    exit 1
fi

cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DVELYX_CONSOLE="$CONSOLE"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo
echo "  $BUILD_DIR/bin/Velyx.exe   everything, in one file"
echo
