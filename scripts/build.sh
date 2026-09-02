#!/bin/bash
# Configure and build ReconOS.
#
#   scripts/build.sh          incremental build
#   scripts/build.sh --clean  wipe the build directory first

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build"

if [ "$1" = "--clean" ]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

cmake -S "$REPO_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD_DIR"

echo
echo "Built: $BUILD_DIR/ReconOS"
echo "Run it from a TTY with: scripts/run.sh"
