#!/usr/bin/env bash
# Start SMail in demo mode on a virtual display once per style, take a
# screenshot, and fail on GTK / libadwaita warnings (CSS errors, criticals).
#   tests/smoke-gui.sh <path-to-smail> [output-dir]
# Needs xvfb-run, ImageMagick (import) and python3.
set -uo pipefail

BIN="$(realpath "$1")"
OUT="$(realpath -m "${2:-smoke}")"
mkdir -p "$OUT"
DBUS=""
command -v dbus-run-session >/dev/null && DBUS="dbus-run-session --"
fail=0

for theme in system matte-white matte-black; do
  profile="$(mktemp -d)"
  mkdir -p "$profile/data/smail"
  python3 - "$profile/data/smail/smail.db" "$theme" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT)")
db.execute("INSERT INTO settings VALUES ('theme', ?)", (sys.argv[2],))
db.commit()
PY
  XDG_DATA_HOME="$profile/data" XDG_CONFIG_HOME="$profile/config" \
  GTK_A11Y=none NO_AT_BRIDGE=1 GSK_RENDERER=cairo \
    xvfb-run -a -s "-screen 0 1280x800x24" $DBUS bash -c '
      "$0" --demo & pid=$!
      sleep 8
      kill -0 $pid || { echo "SMAIL-SMOKE: app exited early"; exit 1; }
      import -window root "$1" || echo "SMAIL-SMOKE: screenshot failed"
      kill $pid; wait $pid 2>/dev/null; exit 0' "$BIN" "$OUT/$theme.png" > "$OUT/$theme.log" 2>&1
  status=$?

  # does the style really apply? check window brightness and the
  # blue-tinted selection the Matte styles give the selected Inbox row
  look=""
  if [ -f "$OUT/$theme.png" ] && command -v convert >/dev/null; then
    mean="$(convert "$OUT/$theme.png" -crop 1170x750+5+5 -colorspace Gray -format '%[fx:int(100*mean)]' info:)"
    tint="$(convert "$OUT/$theme.png" -format '%[fx:int(255*(p{180,103}.b-p{180,103}.r))]' info:)"
    look="brightness ${mean}%, selection blue tint ${tint}"
    case "$theme" in
      matte-white) [ "$mean" -ge 85 ] && [ "$tint" -ge 15 ] || look="$look  <- expected light with a blue selection" ;;
      matte-black) [ "$mean" -le 25 ] && [ "$tint" -ge 15 ] || look="$look  <- expected dark with a blue selection" ;;
    esac
    case "$look" in *expected*) echo "SMAIL-SMOKE: $theme $look" >> "$OUT/$theme.log" ;; esac
  fi

  problems="$(grep -E 'CRITICAL|Theme parser|Gtk-WARNING|Adwaita-WARNING|SMail-WARNING|SMAIL-SMOKE|Segmentation|Aborted' "$OUT/$theme.log" \
              | grep -vE 'a11y|accessibility|org\.freedesktop\.(Notifications|portal)|xdg-desktop-portal|DRI3' || true)"
  if [ $status -ne 0 ] || [ -n "$problems" ]; then
    echo "✗ $theme"
    echo "$problems"
    fail=1
  else
    echo "✓ $theme ($look)"
  fi
  rm -rf "$profile"
done
exit $fail
