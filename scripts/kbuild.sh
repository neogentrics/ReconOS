#!/usr/bin/env bash
# Build one architecture and boot it with a disk attached, in one step.
#
# Exists because the fast loop is build-then-run and the two halves have
# different working directories; typing it out each time is how a run against a
# stale binary happens.
set -eu
cd "$(dirname "$0")/.."

ARCH=${1:-aarch64}

make -C kernel "ARCH=$ARCH" >/dev/null
exec bash scripts/try-disk.sh "$ARCH"
