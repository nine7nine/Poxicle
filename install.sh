#!/usr/bin/env bash
# Full install for Poxicle -> /usr, in one shot:
#   * the simulation engine (embedded — see note below)
#   * the GTK4 configurator            (poxicle-config)
#   * the KWin effect + its Configure KCM
#
# Two build systems by necessity: the engine + configurator are Meson; the KWin
# effect is CMake/ECM (KDE mandates it). Both consumers compile the engine
# straight from src/, so "the engine" ships *inside* each one — there is no
# separate libpoxicle to install. Dedicated build-install dirs are used so the
# dev builds (build/, run-nested.sh) are left untouched.
#
# NOTE: the effect .so does NOT hot-reload. Log out and back in after this to
# load the new effect/engine code. kwinrc changes (palettes, per-app rules) made
# in poxicle-config still apply live via KWin reconfigure.
set -e
cd "$(dirname "$0")"

# Always build CLEAN. The KWin effect bakes the compositor's version into its
# plugin factory IID (effect.h: EffectPluginFactory_iid =
# "org.kde.kwin.EffectPluginFactory" + KWIN_PLUGIN_VERSION_STRING). When KWin
# updates, pacman restores config-kwin.h with the *package build* mtime, which is
# OLDER than the cached .o from the last build — so an incremental rebuild sees
# the object as up to date, skips the recompile, and reinstalls a plugin whose
# IID still names the OLD KWin. The updated compositor then silently refuses to
# load it (it never even appears in Desktop Effects). Wiping the build dirs is
# the only reliable cure, so do it unconditionally.
rm -rf build-install kwin/build-install

echo ">> [1/2] engine + GTK4 configurator  (Meson -> /usr)"
meson setup build-install --prefix=/usr -Dconfigurator=true -Ddemos=false
meson compile -C build-install
echo ">> installing poxicle-config to /usr/bin (sudo)..."
sudo meson install -C build-install

echo
echo ">> [2/2] KWin effect + Configure KCM  (CMake -> /usr)"
# Force the Qt6 plugin dir KWin actually scans on Arch (/usr/lib/qt6/plugins);
# KDE_INSTALL_PLUGINDIR can otherwise resolve to lib/qt/plugins, which KWin never
# looks in, so the effect would never show up.
PLUGINDIR=lib/qt6/plugins
cmake -S kwin -B kwin/build-install -DCMAKE_INSTALL_PREFIX=/usr -DKDE_INSTALL_PLUGINDIR="$PLUGINDIR"
cmake --build kwin/build-install
echo ">> installing effect + KCM to /usr/$PLUGINDIR (sudo)..."
sudo cmake --install kwin/build-install

# Drop stale copies from the earlier wrong plugin path (pre-fix installs).
sudo rm -f /usr/lib/qt/plugins/kwin/effects/plugins/poxicle_kwin.so \
           /usr/lib/qt/plugins/kwin/effects/configs/kwin_poxicle_config.so 2>/dev/null || true

# Refresh KDE's plugin/service cache so System Settings lists the effect.
kbuildsycoca6 >/dev/null 2>&1 || true

cat <<'EOF'

>> installed:
     /usr/bin/poxicle-config                                            (configurator)
     /usr/lib/qt6/plugins/kwin/effects/plugins/poxicle_kwin.so          (effect + engine)
     /usr/lib/qt6/plugins/kwin/effects/configs/kwin_poxicle_config.so   (Configure KCM)

>> enable the effect (first install only):
     kwriteconfig6 --file kwinrc --group Plugins --key poxicle_kwinEnabled true

>> LOG OUT and back in to load the new effect/engine code (the .so does not
>> hot-reload). After that, palette/rule changes from poxicle-config apply live.
EOF
