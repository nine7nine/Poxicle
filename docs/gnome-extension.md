# poxicle — The GNOME Shell Extension

The GNOME Shell extension (`gnome/poxicle@nine7nine.github.com/`) is the GNOME port of the particle ring: a gjs extension that drives the *same* C engine through `gi://Poxicle`, draws on the GPU with Cogl, and receives producer streams over the same bridge as the KWin effect.

## Table of Contents

1. [What the extension is](#1-what-the-extension-is)
2. [Loading the engine via gi://Poxicle](#2-loading-the-engine-via-gipoxicle)
3. [GPU drawing with a Cogl actor](#3-gpu-drawing-with-a-cogl-actor)
4. [The active-window overlay](#4-the-active-window-overlay)
5. [The panel ring](#5-the-panel-ring)
6. [The stream receiver](#6-the-stream-receiver)
7. [Minimize gating and the un-minimize grace](#7-minimize-gating-and-the-un-minimize-grace)
8. [Config, clocks, and fullscreen](#8-config-clocks-and-fullscreen)
9. [Design rules](#9-design-rules)

---

## 1. What the extension is

The extension plays two roles, both backed by one engine so there is no JS sim to keep in sync with the C one:

- an **ambient self-sim** that follows the focused window and overlays a particle ring around its frame, and
- a **bridge receiver** that owns `org.ninez.PoxicleBridge`, so a producer (e.g. Chiguiro) can stream ready-to-draw instances onto its own window under GNOME exactly as it does under KWin.

| Property | Value |
| --- | --- |
| UUID | `poxicle@nine7nine.github.com` |
| Target | GNOME Shell 50 (ESM extensions), Wayland or X11 |
| Engine | `gi://Poxicle` — the same `libpoxicle` C engine the KWin effect links |
| Frame clock | one `GLib.timeout_add` at `TICK_MS = 16` (~60 fps) drives both roles |

It mirrors the KWin effect's behaviour deliberately: the same per-object seeds, the same fullscreen skip, the same un-minimize grace, the same panel edge-mask logic, and the same bridge protocol. (The extension's `README.md` describes an older Cairo-based prototype; the shipping code is the GPU/Cogl implementation described here.)

## 2. Loading the engine via gi://Poxicle

The extension imports the engine like any GObject-Introspection namespace and constructs engines directly:

```js
import Pox from 'gi://Poxicle';
const engine = new Pox.Engine();
```

This is the whole point of the [GObject binding](gobject-binding.gen.html): GNOME and KWin run the identical simulation, so a preset or palette looks the same on both. The `Poxicle-1.0.typelib` is found at runtime simply by being installed under the system prefix where gjs's introspection repository searches — there is no hard-coded path in the extension (see [Build, Install & Packaging](build.gen.html)). The other imports are stock — `GLib`, `Gio`, `Meta`, `Clutter`, `Cogl`, `GObject`, `St`, plus the Shell ESM `Extension` and `Main`.

## 3. GPU drawing with a Cogl actor

Drawing is entirely on the GPU. A per-frame, window-sized Cairo surface — the obvious approach — stalled interactive resize, so the extension instead uploads a triangulated vertex blob and draws it with a Cogl pipeline. `PoxicleParticleActor` is a `Clutter.Actor` subclass with `reactive: false` (clicks pass through to the window) and a custom `vfunc_paint`:

1. It takes this frame's blob from the engine via [`tick_vertices`](gobject-binding.gen.html#3-two-tick-outputs-instances-and-vertices) — interleaved `P2C4` vertices (`float x, y` + 4 premultiplied `u8 r, g, b, a`, 12-byte stride).
2. In `vfunc_paint` it gets the framebuffer and Cogl context, lazily builds a `Cogl.Pipeline` with a premultiplied "over" blend string (`RGBA = ADD(SRC_COLOR, DST_COLOR*(1-SRC_COLOR[A]))`), wraps the blob in a `Cogl.AttributeBuffer`, defines two `Cogl.Attribute`s (`cogl_position_in` at offset 0, `cogl_color_in` at offset 8), and draws one `Cogl.Primitive` of `TRIANGLES`.

That is a **single draw call per frame** through Cogl's stock position+colour pipeline — no JS-authored shaders, no Cairo, no per-frame surface. The premultiplied colours come straight from the C side, matching the blend string. If a GPU paint ever throws, the actor disables itself and logs once, to avoid per-frame log spam.

## 4. The active-window overlay

On enable, the extension connects `global.display`'s `notify::focus-window` to a retarget. `_retarget()` untracks the previous window, then for a normal focused window applies the look with `engine.apply_config(win.get_wm_class())` (which resolves the [neutral config](configuration.gen.html) in C), parents the actor under `global.window_group` as a sibling of window actors, and places it. `_place()` reads `win.get_frame_rect()`, sizes the actor and the engine surface to it, and treats the frame-local origin as the engine origin. The overlay tracks `position-changed` and `size-changed` to re-place, and `unmanaged` to untrack. Corner rounding (GNOME rounds all four corners) is driven by the engine from the config's corner keys.

## 5. The panel ring

The GNOME top bar is chrome, not a `Meta.Window`, so the panel ring is a **separate engine and actor on its own clock**, added directly to `Main.layoutManager.uiGroup`. `_retargetPanel()` applies the `Panel` target via `engine.apply_panel_config()` (which deliberately keeps **sharp corners**). Panel geometry is polled each tick from `Main.layoutManager.panelBox`: if the shell has hidden the panel (e.g. under a fullscreen window) the actor is hidden; otherwise the actor and engine surface are resized to the panel allocation and the edge mask is recomputed.

`_applyPanelEdgeMask()` lights only the edges that face the screen interior, using the primary monitor geometry and a 1 px epsilon: a top full-width bar lights its bottom edge only; a centred or floating panel adds its sides. This is the exact parity with the [KWin effect's panel handling](kwin-effect.gen.html#5-eligibility-panels-and-edge-masks), down to the edge-mask semantics, so a `Panel` rule looks the same on both desktops.

## 6. The stream receiver

The extension owns `org.ninez.PoxicleBridge` on the session bus, exporting `Register` / `Unregister` / `Wake`. `Register` must be handled **asynchronously** (`RegisterAsync`) to reach the message's Unix fd list — the plain wrapped-method path can't carry file descriptors. On register it pulls the fd from the message's fd list, creates a fresh `Pox.Engine`, calls `attach_stream(fd)` (which takes ownership of the fd), and records a per-pid stream with its own actor. Each tick it pulls the latest complete frame with `read_stream_vertices()` and, when that returns non-null, updates the actor's vertices — `null` meaning "no new frame, keep drawing the last," an empty blob meaning "clear." `Wake` is a deliberate no-op kept for protocol parity: the per-tick clock already polls every stream, so an idle→active edge needs no explicit un-park. The C side of all this is the binding's [seqlock reader](gobject-binding.gen.html#6-the-stream-receiver).

A streaming window still gets the focus overlay drawn additively on top — a stream replaces only that window's *own* per-app look, never the focus ring — which matches the KWin effect's behaviour.

## 7. Minimize gating and the un-minimize grace

Because the overlay actors are siblings of the window actor in `window_group`, Mutter's minimize-hide does not cover them — without gating, a ring would freeze mid-air where the window used to be. So for **stream** windows, each tick checks `win.minimized`: while minimized (or fullscreen, or inside the grace window) the actor is hidden and the frame is skipped; on the show-after-restore edge the overlay is re-aimed at the settled frame rectangle.

On the un-minimize edge the extension arms a suppression window of `UNMINIMIZE_GRACE` milliseconds, **scaled by the user's animation speed** (`St.Settings.slow_down_factor`) and collapsing to zero when animations are disabled. This holds the ring through the un-minimize so it doesn't snap to the settled rectangle while Magic Lamp / Burn My Windows is still warping the window up from the panel — the same idea as the [KWin effect's grace](kwin-effect.gen.html#7-the-un-minimize-grace). The grace defaults to 350 ms and is read live from the config's `UnminimizeGrace` key (clamped 0–2000) on enable and on every config change.

## 8. Config, clocks, and fullscreen

The extension does not parse the config itself — it **watches** it. `~/.config/poxicle/poxicle.conf` is monitored with a `Gio.FileMonitor`; on change it re-reads the grace and re-applies both the focus and panel looks. The actual resolution happens in C via `apply_config` / `apply_panel_config`, so GNOME and KWin honour byte-identical edits (see [Configuration & Presets](configuration.gen.html)).

Two more parities with the effect round it out. Each independently-simulated engine is given a unique seed from a monotonic counter (`set_seed`) so the focus ring and the panel ring don't animate in lockstep. And the ambient overlay skips **fullscreen** windows each tick (`win.is_fullscreen()` → hide), since a ring would draw over edge-to-edge content; the panel honours fullscreen implicitly because the shell hides the panel box.

## 9. Design rules

- **One engine, two desktops.** The extension drives the same C engine through `gi://Poxicle`, so every preset, palette and rule renders identically under GNOME and KWin.
- **Draw on the GPU.** A Cogl actor uploads a premultiplied vertex blob and draws it in one primitive per frame — no Cairo, no per-frame surface, no resize stall.
- **Watch the config, resolve in C.** The extension only monitors the neutral file; `apply_config` does the parsing, so there is no second config parser to drift.
- **Mirror the effect.** Per-object seeds, fullscreen skip, the un-minimize grace, panel edge masks, and the bridge protocol all match the KWin effect on purpose.
- **Gate siblings explicitly.** Overlay actors aren't hidden by Mutter's minimize, so the extension hides them itself and holds them through the un-minimize warp.
