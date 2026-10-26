#!/bin/bash
# Builds the universal (Apple Silicon + Intel) filter and the installer package.
# Output: dist/ZebraCrisp-<version>.pkg
set -euo pipefail
cd "$(dirname "$0")"

VERSION="${1:-1.0.0}"
BUILD=build
ROOT="$BUILD/root"

rm -rf "$BUILD" dist
mkdir -p "$ROOT/Library/Printers/ZebraCrisp" "$ROOT/Library/Printers/PPDs/Contents/Resources" dist

clang -O2 -Wall -Wno-deprecated-declarations -arch arm64 -arch x86_64 \
  -mmacosx-version-min=11.0 -o "$ROOT/Library/Printers/ZebraCrisp/rastertozebracrisp" \
  src/rastertozebracrisp.c -lcups

cp ppd/ZebraCrisp.ppd "$ROOT/Library/Printers/PPDs/Contents/Resources/ZebraCrisp.ppd"
cp scripts/uninstall.sh "$ROOT/Library/Printers/ZebraCrisp/uninstall.sh"
chmod 755 "$ROOT/Library/Printers/ZebraCrisp/"*
chmod 644 "$ROOT/Library/Printers/PPDs/Contents/Resources/ZebraCrisp.ppd"

mkdir -p "$BUILD/scripts"
cp scripts/postinstall "$BUILD/scripts/postinstall"
chmod 755 "$BUILD/scripts/postinstall"

xattr -cr "$ROOT" "$BUILD/scripts"
export COPYFILE_DISABLE=1
pkgbuild --root "$ROOT" --scripts "$BUILD/scripts" \
  --identifier com.github.jcl5m1.zebra-crisp-mac --version "$VERSION" \
  --install-location / --ownership recommended \
  "dist/ZebraCrisp-$VERSION.pkg"

echo "Built dist/ZebraCrisp-$VERSION.pkg"
