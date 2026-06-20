#!/usr/bin/env bash
# Build poxicle_kwin and run it in an ISOLATED nested KWin (its own D-Bus session
# and config dir). Safe: it cannot touch your live desktop or kwinrc. Close the
# nested window — or Ctrl-C this terminal — to end the test. Shader/GL errors from
# poxicle print to this terminal's stderr.
set -e
cd "$(dirname "$0")"

# Always reconfigure with an explicit -S (safe, picks up CMakeLists/source
# changes). Avoid the bare `cmake -B build`, which on a git-init'd repo can leave
# epoxy unresolved ("No rule to make target 'epoxy'").
echo ">> configuring + building..."
cmake -S . -B build
cmake --build build
echo ">> built build/bin/kwin/effects/plugins/poxicle_kwin.so"

CFG=/tmp/pox-kwin-test
mkdir -p "$CFG"
kwriteconfig6 --file "$CFG/kwinrc" --group Plugins --key poxicle_kwinEnabled true

# Demo per-app config so the resolution path is visible: konsole gets the
# orange 'fireworks' preset, everything else the default 'ambient'. Edit/remove
# to test other rules. (Rules is a StringList; comma-separate multiple rules.)
kwriteconfig6 --file "$CFG/kwinrc" --group "Effect-poxicle_kwin" --key DefaultPreset ambient
kwriteconfig6 --file "$CFG/kwinrc" --group "Effect-poxicle_kwin" --key Rules "konsole|fireworks|0|#ff6600"

echo ">> launching nested KWin (1600x1000) — close the window to quit"
QT_PLUGIN_PATH="$PWD/build/bin" XDG_CONFIG_HOME="$CFG" \
  dbus-run-session -- kwin_wayland --width 1600 --height 1000 konsole
