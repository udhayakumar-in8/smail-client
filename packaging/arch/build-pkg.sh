#!/usr/bin/env bash
# Build the Arch package from the current checkout (used by CI and releases).
#   packaging/arch/build-pkg.sh <source-tarball> [output-dir]
# Must run on Arch Linux as a user that may run makepkg (not root).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TARBALL="$(realpath "$1")"
OUT="$(realpath -m "${2:-$HERE/../../dist}")"
NAME="$(basename "$TARBALL")"                 # smail-client-<version>.tar.xz
VERSION="${NAME#smail-client-}"; VERSION="${VERSION%.tar.xz}"
SUM="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# PKGBUILD with the real version and checksum; makepkg uses the local tarball
sed -e "s/^pkgver=.*/pkgver=$VERSION/" \
    -e "s/^sha256sums=.*/sha256sums=('$SUM')/" "$HERE/PKGBUILD" > "$WORK/PKGBUILD"
cp "$TARBALL" "$WORK/"

(cd "$WORK" && makepkg --cleanbuild --force --noconfirm)

mkdir -p "$OUT"
cp "$WORK"/smail-*.pkg.tar.zst "$OUT/"
cp "$WORK/PKGBUILD" "$OUT/PKGBUILD"
ls "$OUT"/smail-*.pkg.tar.zst
