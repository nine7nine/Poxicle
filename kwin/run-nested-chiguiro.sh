#!/usr/bin/env bash
# Like run-nested.sh, but the nested KWin launches the devel **Chiguiro** (the
# compositor-render producer) instead of konsole — so you actually test the
# Chiguiro -> poxicle-kwin external-source stream, not a stand-in app.
#
# It builds + loads poxicle_kwin in an ISOLATED nested KWin (own D-Bus session +
# config dir): cannot touch your live desktop or kwinrc, and (crucially on
# Wayland) needs NO logout to pick up a recompiled effect .so. Close the konsole
# window or Ctrl-C this terminal to end the test. GL/shader errors print here.
#
# Chiguiro is started with KGX_POXICLE=compositor so it streams to the effect
# regardless of its stored setting. A konsole is launched alongside it as a
# shell INSIDE the nested session, so you can relaunch chiguiro-devel (to retry,
# or trigger more bells/process-exits) without restarting the whole nested KWin.
set -e
cd "$(dirname "$0")"

# Where Chiguiro's devel build lives (override by exporting before running).
CHIGUIRO_BUILDDIR="${CHIGUIRO_BUILDDIR:-/home/ninez/Github/Chiguiro-codex/builddir}"
CHIGUIRO_BIN="$CHIGUIRO_BUILDDIR/src/chiguiro-devel"
CHIGUIRO_SCHEMAS="$CHIGUIRO_BUILDDIR/data"

if [ ! -x "$CHIGUIRO_BIN" ]; then
  echo "!! chiguiro-devel not found at $CHIGUIRO_BIN" >&2
  echo "   build it first (meson compile -C \"$CHIGUIRO_BUILDDIR\") or set CHIGUIRO_BUILDDIR." >&2
  exit 1
fi

echo ">> configuring + building the effect..."
cmake -S . -B build
cmake --build build
echo ">> built build/bin/kwin/effects/plugins/poxicle_kwin.so"

CFG=/tmp/pox-kwin-test
mkdir -p "$CFG"
kwriteconfig6 --file "$CFG/kwinrc" --group Plugins --key poxicle_kwinEnabled true
# NO per-app Rules: with none, the effect's own-sim draws on nothing, so every
# particle you see on the Chiguiro window comes through its external-source
# stream — exactly the Phase 2 path we're validating.
kwriteconfig6 --file "$CFG/kwinrc" --group "Effect-poxicle_kwin" --key DefaultPreset none

# kwin_wayland parses any leading-dash args (e.g. `bash -c ...`) as its OWN
# options, so hand it a single bare executable instead: a launcher script that
# starts Chiguiro and a konsole. It inherits the env exported below.
LAUNCHER="$CFG/launch-chiguiro.sh"
cat > "$LAUNCHER" <<EOF
#!/usr/bin/env bash
# Auto-run inside the nested KWin. Chiguiro (compositor backend) in the
# background; konsole as the foreground shell — close it (or Ctrl-C the outer
# terminal) to end the session. Relaunch Chiguiro from konsole with: chiguiro-devel
"$CHIGUIRO_BIN" &
exec konsole
EOF
chmod +x "$LAUNCHER"

echo ">> launching nested KWin (1600x1000) running chiguiro-devel (compositor backend)"
echo "   close the konsole window or Ctrl-C to quit; relaunch Chiguiro from konsole with: chiguiro-devel"
QT_PLUGIN_PATH="$PWD/build/bin" XDG_CONFIG_HOME="$CFG" \
GSETTINGS_SCHEMA_DIR="$CHIGUIRO_SCHEMAS" KGX_POXICLE=compositor \
PATH="$CHIGUIRO_BUILDDIR/src:$PATH" \
  dbus-run-session -- kwin_wayland --width 1600 --height 1000 "$LAUNCHER"
