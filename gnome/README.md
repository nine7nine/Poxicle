# Poxicle — GNOME Shell extension

The GNOME Shell port of the Poxicle KWin effect: draws edge particles around the
**active window**. GNOME Shell 50+ (ESM extensions), Wayland or X11.

## Install (dev)

```bash
gnome/install.sh        # builds+installs the GI binding (sudo), symlinks the extension, enables it
# then log out / back in  (Wayland can't hot-load a new extension)
```

`install.sh` does two things: builds the engine's `-Dintrospection` target
(`libpoxicle-gobject` + `Poxicle-1.0.typelib`) and installs it to a system prefix
(needs sudo) so gnome-shell's gjs can `import gi://Poxicle`; then symlinks the
extension into `~/.local/share/gnome-shell/extensions` and enables it.

Watch for errors: `journalctl --user -f -o cat /usr/bin/gnome-shell`

## Status

**Working (renders real particles):** `extension.js` tracks
`global.display.focus_window`, overlays a transparent `St.DrawingArea` over the
window's `get_frame_rect()`, follows move/resize, runs a ~60fps loop, and draws
each instance with Cairo (square / circle / diamond / triangle + clamped alpha).
A default preset (`ambient` + the Verdant palette) is applied to the focused window.

**The engine is the real C `libpoxicle`, via GObject-Introspection** — not a JS
port. `extension.js` does `import gi://Poxicle`, `new Pox.Engine()`, and reads
`tick()`'s packed instance blob with a `DataView`. So `src/poxicle.c` changes flow
to GNOME by rebuilding the binding — **nothing to hand-sync**. (KWin links the C
engine directly because effects are native; GNOME extensions are JS, so GI is the
equivalent way to use the same library.)

**Then — config the GNOME way (not kwinrc).** Following the GNOME extension
conventions: a GSettings schema + an Adwaita `prefs.js` preferences window for
enable/preset/palette/per-app rules. (The KWin effect reading `kwinrc` is the KDE
integration; this is the standalone GNOME citizen — its own GSettings store.)

**Feature parity:** palettes, reverse/implode, the 19 presets are all already in
the engine — `set_preset()` / `set_palette()` select them. Later: optionally the
`org.ninez.PoxicleBridge` so Chiguiro can stream to GNOME too (over a gjs-friendly
transport, since gjs can't mmap the producer's shm).

## Layout

- `poxicle@nine7nine.github.com/metadata.json` — extension manifest (shell-version 50).
- `poxicle@nine7nine.github.com/extension.js` — window tracking + overlay + frame loop + Cairo draw.
- The engine + GI binding live in `../src` (`pox-gobject.c`) and install as `gi://Poxicle`.
