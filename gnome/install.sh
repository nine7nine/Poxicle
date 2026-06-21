#!/usr/bin/env bash
# Install the Poxicle GNOME Shell extension.
#
# Two parts:
#   1. The engine's GObject/GI binding (gi://Poxicle) — a shared lib + typelib —
#      installed to a system prefix so gnome-shell's gjs can import it. This is
#      the SAME C engine the KWin effect uses; no separate JS copy.
#   2. The extension itself — symlinked into the user extensions dir + enabled.
#
# On Wayland you must log out/in afterwards (gnome-shell can't hot-load on Wayland).
set -euo pipefail

UUID="poxicle@nine7nine.github.com"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
PREFIX="${PREFIX:-/usr}"   # GI searches the system prefix; override with PREFIX=...

echo "== Building the engine GI binding (-Dintrospection) =="
meson setup "$REPO/build-gir" "$REPO" \
    -Dintrospection=true -Ddemos=false -Dconfigurator=false --prefix="$PREFIX" \
    >/dev/null 2>&1 || meson configure "$REPO/build-gir" -Dintrospection=true --prefix="$PREFIX"
ninja -C "$REPO/build-gir"

echo "== Installing libpoxicle-gobject + Poxicle-1.0.typelib to $PREFIX (sudo) =="
sudo meson install -C "$REPO/build-gir" --only-changed

echo "== Linking the extension =="
DEST="$HOME/.local/share/gnome-shell/extensions/$UUID"
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
ln -s "$HERE/$UUID" "$DEST"
echo "Linked $DEST -> $HERE/$UUID"

gnome-extensions enable "$UUID" 2>/dev/null || echo "Enable after relogin: gnome-extensions enable $UUID"
echo
echo "Log out and back in to load it (Wayland can't hot-load a new extension)."
echo "Watch for errors: journalctl --user -f -o cat /usr/bin/gnome-shell"
