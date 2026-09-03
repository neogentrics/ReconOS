#!/bin/bash
# Build a ReconOS release and wrap it up for installing somewhere else.
#
#   scripts/package.sh
#
# Produces dist/reconos-<version>-<arch>.tar.gz containing the compositor, its
# assets, and an installer. The result installs onto a machine that already has
# a Linux kernel and wlroots; it is not a bootable disk image, and says so
# rather than pretending otherwise.
#
# What it is for: taking a build off the machine it was compiled on and
# starting it on a fresh one without a copy of the source or a toolchain.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build"
DIST_DIR="$REPO_DIR/dist"

# The project's version, not cmake_minimum_required's -- which is the first
# "VERSION <number>" in the file and is not what anyone means by "the version".
VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9.]*\)[[:space:]]*$/\1/p' \
    "$REPO_DIR/CMakeLists.txt" | head -1)"

if [ -z "$VERSION" ]; then
    echo "Could not read the version out of CMakeLists.txt." >&2
    exit 1
fi
ARCH="$(uname -m)"
NAME="reconos-${VERSION}-${ARCH}"
STAGE="$DIST_DIR/$NAME"

echo "Packaging ReconOS $VERSION for $ARCH"

# Always build fresh: shipping whatever happened to be in build/ is how a
# release ends up being something nobody can reproduce.
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

if [ ! -x "$BUILD_DIR/ReconOS" ]; then
    echo "The build did not produce a binary." >&2
    exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/share"

cp "$BUILD_DIR/ReconOS" "$STAGE/bin/"
strip "$STAGE/bin/ReconOS" 2>/dev/null || true

if [ -d "$REPO_DIR/assets" ]; then
    cp -r "$REPO_DIR/assets" "$STAGE/share/"
fi

cp "$REPO_DIR/README.md" "$REPO_DIR/LICENSE.txt" "$STAGE/" 2>/dev/null || true
cp "$REPO_DIR/THIRD_PARTY.md" "$STAGE/" 2>/dev/null || true

cp "$REPO_DIR/scripts/install.sh" "$STAGE/install.sh"
chmod +x "$STAGE/install.sh"

# Record what this was built against. When a copied binary refuses to start on
# another machine, this is the first thing worth comparing.
{
    echo "ReconOS $VERSION"
    echo "architecture: $ARCH"
    echo "built on:     $(uname -sr)"
    echo "wlroots:      $(pkg-config --modversion wlroots 2>/dev/null || echo unknown)"
    echo "wayland:      $(pkg-config --modversion wayland-server 2>/dev/null || echo unknown)"
} > "$STAGE/BUILD-INFO.txt"

tar -czf "$DIST_DIR/$NAME.tar.gz" -C "$DIST_DIR" "$NAME"
rm -rf "$STAGE"

echo
echo "Wrote $DIST_DIR/$NAME.tar.gz"
echo
echo "On the target machine:"
echo "  tar -xzf $NAME.tar.gz"
echo "  cd $NAME"
echo "  sudo ./install.sh"
