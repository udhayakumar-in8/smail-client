#!/usr/bin/env bash
# Build a .deb of SMail for the Debian/Ubuntu release this runs on.
#   packaging/build-deb.sh [output-dir]
# Needs the build dependencies from the README plus dpkg-dev.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

VERSION="$(meson introspect --projectinfo "$ROOT/meson.build" 2>/dev/null \
            | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')"
ARCH="$(dpkg --print-architecture)"
DIST="$(. /etc/os-release && echo "${ID}${VERSION_ID}")"
PKG="$WORK/pkg"

meson setup "$WORK/build" "$ROOT" --prefix=/usr --buildtype=release -Db_ndebug=true >/dev/null
meson compile -C "$WORK/build"
meson test -C "$WORK/build" --print-errorlogs
DESTDIR="$PKG" meson install -C "$WORK/build" --no-rebuild >/dev/null
strip --strip-unneeded "$PKG/usr/bin/smail"

install -Dm644 "$ROOT/README.md" "$PKG/usr/share/doc/smail/README.md"
install -Dm644 "$ROOT/LICENSE"   "$PKG/usr/share/doc/smail/copyright"

# Work out the shared-library dependencies from the binary itself.
mkdir -p "$WORK/debian"
printf 'Source: smail\n\nPackage: smail\nArchitecture: any\n' > "$WORK/debian/control"
SHLIBS="$(cd "$WORK" && dpkg-shlibdeps -O "$PKG/usr/bin/smail" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"

mkdir -p "$PKG/DEBIAN"
cat > "$PKG/DEBIAN/control" <<CONTROL
Package: smail
Version: ${VERSION}-1~${DIST}
Architecture: ${ARCH}
Maintainer: Udhayakumar <udhayazm1@gmail.com>
Installed-Size: $(du -sk "$PKG/usr" | cut -f1)
Depends: ${SHLIBS}, glib-networking
Recommends: gnome-keyring | org.freedesktop.secrets
Section: mail
Priority: optional
Homepage: https://github.com/udhayakumar-in8/smail-client
Description: email client with calendar, tasks and local chat
 SMail is a GTK 4 / libadwaita email client in the style of elementary OS
 Mail: IMAP and SMTP, mail filters, an automatic mail log, calendar, tasks,
 reminders, chat over the local network, backup and restore, and Matte
 White / Matte Black styles.
CONTROL

mkdir -p "$OUT"
# "~" is right in the version field but GitHub renames it in release
# downloads, so keep it out of the file name (and so out of SHA256SUMS)
DEB="$OUT/smail_${VERSION}-1.${DIST}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$PKG" "$DEB" >/dev/null
echo "$DEB"
