# poxicle-kwin

A **KWin (Plasma 6, Wayland-only) binary OpenGL effect** that draws
[poxicle](https://github.com/) edge particles per window. It reuses poxicle's
simulation core and GLES renderer through the bring-your-own-context path
(`pox_gl_render_mvp`), driven by KWin's own projection matrix — no subsurface,
no EGL, no `poxicle-wl`. KWin owns the surface and the GL context.

This is the *compositor* consumer of poxicle; the in-app overlay
(`poxicle-wl` / byoc) is unchanged. Full design, rationale, milestones, and the
KWin 6.6.5 API notes live in **`poxicle/docs/kwin-effect-handoff.md`**.

> ⚠️ Status: **early scaffold (M1).** It compiles and links against the installed
> KWin. Visual behaviour needs verification in a nested KWin (see below). Not yet
> wired to KConfig; tunables are poxicle defaults.

## Build

`poxicle/` is a symlink to a local poxicle checkout (its core C files are
compiled straight in). Targets the installed KWin — the effect API has **no
binary compatibility**, so rebuild whenever KWin updates.

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
```

## Develop / test safely (never on the live session)

A crash in the live compositor takes your whole desktop. Iterate in a **nested
KWin** on its own D-Bus session and config dir, loading the plugin from the
build dir without installing. ("Windowed mode" is implied by `--width/--height`;
there is no `--windowed` flag in KWin 6.)

```bash
# isolated throwaway config, effect pre-enabled
mkdir -p /tmp/pox-kwin-test
kwriteconfig6 --file /tmp/pox-kwin-test/kwinrc --group Plugins --key poxicle_kwinEnabled true

# nested KWin: own dbus + own config, plugin loaded from build/
QT_PLUGIN_PATH="$PWD/build/bin" XDG_CONFIG_HOME=/tmp/pox-kwin-test \
  dbus-run-session -- kwin_wayland --width 1600 --height 1000 konsole
```

Run it from a terminal to see stderr (poxicle-gl prints shader errors there).
Nothing here touches your real `kwinrc` or the live KWin. Close the nested
window to end the test.

## Install

```bash
sudo cmake --install build
```

Then enable it in System Settings → Desktop Effects, or via the `kwriteconfig6`
line above against your real `kwinrc`.

## License

GPL-3.0-or-later.
