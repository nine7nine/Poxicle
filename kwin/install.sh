#!/usr/bin/env bash
# Build and install the Poxicle KWin effect + its Configure KCM into the live
# system (prefix /usr, so KWin's plugin scanner finds them). Uses a dedicated
# build-install dir so it doesn't disturb the dev build used by run-nested.sh.
#
# After install, enable it: System Settings -> Desktop Effects -> "Poxicle
# Particles" -> (Configure), or the kwriteconfig6 lines printed at the end.
set -e
cd "$(dirname "$0")"

# Force the Qt6 plugin dir KWin actually scans on Arch (/usr/lib/qt6/plugins).
# Without this, KDE_INSTALL_PLUGINDIR can resolve to lib/qt/plugins, which KWin
# never looks in, so the effect never shows up.
PLUGINDIR=lib/qt6/plugins
cmake -S . -B build-install -DCMAKE_INSTALL_PREFIX=/usr -DKDE_INSTALL_PLUGINDIR="$PLUGINDIR"
cmake --build build-install

echo
echo ">> installing to /usr/$PLUGINDIR (sudo)..."
sudo cmake --install build-install

# Remove copies from the earlier wrong path (pre-fix installs landed here).
sudo rm -f /usr/lib/qt/plugins/kwin/effects/plugins/poxicle_kwin.so \
           /usr/lib/qt/plugins/kwin/effects/configs/kwin_poxicle_config.so 2>/dev/null || true

# Refresh KDE's plugin/service cache so System Settings lists the new effect.
kbuildsycoca6 >/dev/null 2>&1 || true

cat <<'EOF'

>> installed:
     /usr/lib/qt6/plugins/kwin/effects/plugins/poxicle_kwin.so
     /usr/lib/qt6/plugins/kwin/effects/configs/kwin_poxicle_config.so

>> enable it:
     kwriteconfig6 --file kwinrc --group Plugins --key poxicle_kwinEnabled true
     qdbus org.kde.KWin /KWin reconfigure

>> It should now appear in System Settings -> Desktop Effects as "Poxicle
>> Particles" (with a Configure button). If the LIST doesn't refresh, log out and
>> back in — but the effect still loads/runs from the enable + reconfigure above.

>> to remove later: delete the two .so files above and disable the plugin.
EOF
