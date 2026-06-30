# poxicle — The KWin Plasma 6 Effect

The KWin effect (`kwin/`) is a standalone Plasma 6 compositor effect that draws poxicle edge particles around each window, by compiling the engine straight into a KWin plugin and feeding the renderer KWin's own projection matrix.

## Table of Contents

1. [What the effect is](#1-what-the-effect-is)
2. [The 6.7 Effect API port](#2-the-67-effect-api-port)
3. [The per-window paint pipeline](#3-the-per-window-paint-pipeline)
4. [GL context and state hygiene](#4-gl-context-and-state-hygiene)
5. [Eligibility, panels, and edge masks](#5-eligibility-panels-and-edge-masks)
6. [Ring-only repaint and idle parking](#6-ring-only-repaint-and-idle-parking)
7. [The un-minimize grace](#7-the-un-minimize-grace)
8. [The poxbridge receiver](#8-the-poxbridge-receiver)
9. [Configuration: the neutral file vs the KCM](#9-configuration-the-neutral-file-vs-the-kcm)
10. [Design rules](#10-design-rules)

---

## 1. What the effect is

`PoxicleEffect` subclasses **`KWin::Effect`** directly — not `OffscreenEffect`. It does not render windows to offscreen textures; it draws particles with raw GL straight into KWin's paint passes, on top of each window's own content. The engine is not a shared library here: the CMake build compiles [`../src/poxicle.c`](simulation-core.gen.html) and [`../src/poxicle-gl.c`](renderer.gen.html) directly into the plugin, with no Wayland/EGL backend, because KWin already owns the surface and the GL context. This makes the effect a [bring-your-own-context host](wayland-backend.gen.html#8-bring-your-own-context).

| Property | Value |
| --- | --- |
| Class / plugin | `PoxicleEffect` → `poxicle_kwin.so` in `kwin/effects/plugins` |
| Factory | `KWIN_EFFECT_FACTORY_SUPPORTED(..., return KWin::effects->isOpenGLCompositing();)` |
| Metadata | Name "Poxicle Particles", Category "Appearance", `EnabledByDefault: false`, GPL-3.0-or-later |
| Configure module | `X-KDE-ConfigModule: kwin_poxicle_config` |
| Chain position | `requestedEffectChainPosition() == 90` — draws over each window's content |
| Target | KWin / Plasma 6 (6.6.5+) |

Because the effect's plugin-factory IID embeds the KWin effect-API version, and that API is **not binary-compatible** across KWin releases, the plugin must be rebuilt whenever KWin bumps its API — which is why the install path always does a clean build (see [Build, Install & Packaging](build.gen.html)).

## 2. The 6.7 Effect API port

The effect was ported from the 6.6.5 design to KWin 6.7's Effect API, which changed several signatures and capabilities:

- **`prePaintScreen` lost its `presentTime` argument.** Present time is now derived from `data.frame->targetPageflipTime()` (a `KWin::OutputFrame`), falling back to the last known present when no frame is attached.
- **Drawing moved from the whole-scene overlay to per-window `paintWindow`.** The original drew every window's particles above the entire scene; the port draws each window's particles in its own pass *after* that window is painted, so they are correctly **occluded by windows stacked above**.
- **`prePaintWindow` gained a leading `RenderView *` parameter.**
- **`WindowPrePaintData::devicePaint` was removed.** That field used to expand a window's paint region so particles could draw outside its bounding scissor. The only remaining way is to mark the window transformed via `data.setTransformed()` — done **only** when the window actually has particles this frame, so idle windows stay on the cheap, region-limited path.

The effect overrides `prePaintScreen`, `prePaintWindow`, `paintWindow`, `postPaintScreen`, `isActive`, and `reconfigure`.

## 3. The per-window paint pipeline

Each frame walks two phases — a screen-wide advance, then a per-window draw:

<div class="diagram-container">
<svg width="100%" viewBox="0 0 920 340" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg    { fill: #1a1b26; }
    .box   { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .pre   { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.5; }
    .paint { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .post  { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .lbl   { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm{ fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut{ fill: #8c92b3; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-blu{ fill: #7aa2f7; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy{ fill: #7dcfff; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-grn{ fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln    { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .title { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="920" height="340" class="bg"/>
  <text x="460" y="26" text-anchor="middle" class="title">one compositor frame</text>

  <rect x="30" y="50" width="380" height="120" class="pre"/>
  <text x="50" y="72" class="lbl-blu">prePaintScreen (once per output)</text>
  <rect x="50" y="84" width="340" height="34" class="box"/>
  <text x="220" y="105" text-anchor="middle" class="lbl-sm">tick each engine: pox_engine_tick -> m_scratch</text>
  <rect x="50" y="124" width="340" height="34" class="box"/>
  <text x="220" y="145" text-anchor="middle" class="lbl-mut">offset by frameGeometry().x()/y() -> fx.instances</text>

  <rect x="510" y="50" width="380" height="120" class="paint"/>
  <text x="530" y="72" class="lbl-cy">paintWindow (per window, after its content)</text>
  <rect x="530" y="84" width="340" height="34" class="box"/>
  <text x="700" y="105" text-anchor="middle" class="lbl-sm">mvp = viewport.projectionMatrix()</text>
  <rect x="530" y="124" width="340" height="34" class="box"/>
  <text x="700" y="145" text-anchor="middle" class="lbl-mut">mvp.translate(data.*Translation); pox_gl_render_mvp</text>

  <rect x="270" y="220" width="380" height="90" class="post"/>
  <text x="290" y="242" class="lbl-grn">postPaintScreen</text>
  <rect x="290" y="254" width="340" height="34" class="box"/>
  <text x="460" y="275" text-anchor="middle" class="lbl-sm">requestRingRepaint: damage 4 edge strips only</text>
  <text x="460" y="300" text-anchor="middle" class="lbl-mut">while m_active; else KWin parks (no repaint requested)</text>

  <line x1="410" y1="110" x2="510" y2="110" class="ln"/>
  <line x1="700" y1="170" x2="460" y2="220" class="ln"/>
</svg>
</div>

In `prePaintScreen`, every tracked engine is advanced with `pox_engine_tick(engine, dt, m_scratch, ...)` (the scratch buffer holds 4096 instances). poxicle emits surface-local pixels, so each instance's `x/y` is offset by the window's `frameGeometry()` to become global logical coordinates and stashed per-window in `fx.instances`. `dt` comes from the difference of present timestamps, and the whole advance is gated `presentTime > m_lastPresent` so a multi-output frame (which fires `prePaintScreen` once per output) never double-advances the sim.

In `paintWindow`, the projection is **KWin's own**: `mvp = viewport.projectionMatrix()` — the logical→clip matrix that already handles output scale and transform, column-major exactly as [`pox_gl_render_mvp`](renderer.gen.html) wants, with no manual Y-flip. The matrix is then `translate`d by the window paint data's per-frame translation so the ring tracks interactive moves: particles are positioned from the *committed* geometry, which lags a drag by a frame or two, and the translation re-applies KWin's in-flight offset so they don't trail. The base layer (a per-app engine or an external stream — mutually exclusive per window) is drawn first, then the focus-following active-window overlay on top.

## 4. GL context and state hygiene

The effect creates its `PoxGL` renderer in the constructor — the framework guarantees a current GL context there. Two pieces of GL hygiene are mandatory, because poxicle-gl sets up and leaves bound its own state and never restores KWin's:

- Around `pox_gl_new()`, the effect saves and restores `GL_VERTEX_ARRAY_BINDING` and `GL_ARRAY_BUFFER_BINDING`. Otherwise the renderer's VAO stays bound and floods KWin's own draws with `GL_INVALID_OPERATION` ("no array object bound").
- Around every `pox_gl_render_mvp` in `paintWindow`, it saves and restores the current program, VAO, array-buffer binding, the blend enable, and the separate blend function — all of which the renderer clobbers.

The window's particle surface is sized with `pox_engine_set_surface(engine, geom.width(), geom.height(), 1)`; the scale hint is hard-coded to 1 because positions stay logical and KWin's projection applies the device scale.

## 5. Eligibility, panels, and edge masks

Two predicates gate everything. `eligible(w)` admits a window only if it is a normal window and not a desktop, dock, or popup. `visible(w)` — `isOnCurrentDesktop && !isMinimized && !isHiddenByShowDesktop && !isFullScreen` — is the single per-frame gate every draw, tick and repaint path runs through. **Fullscreen windows get no ring**, since particles would draw over edge-to-edge content.

A **panel** (a dock) is rejected by `eligible` and admitted only through the dedicated **`Panel` target** in `maybeAttach`: if `resolvePanel()` returns an enabled rule, the dock is tracked with `isPanel = true` and an edge mask. `applyPanelEdgeMask(w)` lights an edge only when it is *not* flush against the matching screen border (a gap greater than `eps = 1.0`, derived from `frameGeometry()` vs the screen geometry). A full-width top panel therefore lights only its bottom edge; a floating panel lights all four. When a tracked panel moves or resizes, `prePaintScreen` recomputes its mask so toggling panel edge or width flips which edges face the interior. Panels keep **sharp corners** — the corner radius is applied only to normal windows, from the config's `CornerTop` / `CornerBottom`.

Every engine is given a unique seed (`pox_engine_set_seed(engine, ++m_seedSeq)`) so multiple rings — the panel, the active overlay, each per-app window — run on [their own clocks](simulation-core.gen.html#7-per-engine-clocks-and-idle-parking) rather than animating in lockstep.

## 6. Ring-only repaint and idle parking

A naive effect would damage each window's whole rectangle every frame. For a translucent (glass) window that forces KWin to recomposite the entire stack behind it across the full window area — cheap for a thin ring, ruinous for a big window. So `requestRingRepaint()` damages **only the four edge strips** the particles occupy: it computes how deep any block reaches in from its nearest edge, then issues four `addRepaint` rectangles (top/bottom/left/right) of that thickness plus a `kBandMargin = 48` px margin. A deep effect grows the band until the strips meet (capped at a full-window band), so the ring is never under-damaged and leaves no trails.

`postPaintScreen` requests these bands for every visible window, the active overlay, and every bound stream — but only while `m_active`, which is reset each frame and set true only when some engine `pox_engine_active()` is true or a stream has instances. When nothing animates, no repaint is requested and **KWin sleeps**. `isActive()` returns false when there are no tracked windows, no active overlay, and no streams, letting KWin drop the effect from the chain entirely.

## 7. The un-minimize grace

Particle rings are siblings of window content, so a window-warp animation (Magic Lamp, Squash, Glide, Burn My Windows) can finish *before* the window settles at its final rectangle — and KWin flips `isMinimized()` to false at the **start** of a restore. Without care the ring would snap to the settled rectangle while the warp is still mid-flight. The effect holds it with a per-window `Restore` record:

- `armRestore(w)` sets a deadline of `max(minimizeAnimMs, unminimizeGraceMs) × animationTimeFactor + 60 ms` and forces full repaints so compositing keeps running until the hold releases. It is armed from KWin's `slotMinimizedChanged` and `slotShowingDesktopChanged(false)` signals.
- An **affine** restore (Squash/Glide — detected by a non-identity scale) releases the instant the scale settles. A **non-affine** restore (Magic Lamp shows no scale) is held until the timed deadline. Foreign affine transforms with no restore pending (overview, desktop grid, maximize) suppress the draw entirely, while interactive move/resize is exempt.

Holds are keyed by window, so they cover per-app, overlay, and stream particles alike, and survive a producer re-registering its stream across the minimize.

## 8. The poxbridge receiver

The effect is the canonical receiver of the [poxbridge protocol](poxbridge.gen.html). In its constructor it best-effort owns `org.ninez.PoxicleBridge` on the session bus (`registerService` + `registerObject(..., ExportScriptableSlots)`); if the name is already taken — say a stale effect instance — external mode is simply unavailable while the per-app and active-window paths keep working. The class's `Q_CLASSINFO("D-Bus Interface", "org.ninez.PoxicleBridge")` must be a literal equal to `POX_BRIDGE_IFACE`, or QtDBus names the interface after the C++ class and producer calls fail.

The three `Q_SCRIPTABLE` methods are `Register(pid, shm)`, `Unregister(pid)`, and `Wake(pid)`. `Register` dups the fd, `fstat`s its real size, maps it read-only, validates the `PoxBridgeHeader`, replaces any prior stream for that pid, stores an `ExtStream` keyed by pid, and binds it. `readStreamFrame` is the [seqlock reader](poxbridge.gen.html#4-the-seqlock) (up to 4 retries); after `kStreamIdleGrace = 4` empty polls the stream clears so a quiet producer lets KWin sleep. A stream **takes precedence** over a window's per-app engine — binding drops any owns-sim state so the two never double-draw. A producer is matched to its window by PID. The bundled `poxbridge-test` executable is a standalone producer that stands in for Chiguiro to exercise this path in a nested KWin.

## 9. Configuration: the neutral file vs the KCM

At runtime the effect reads the **DE-neutral config file** — `~/.config/poxicle/poxicle.conf`, group `[poxicle]` — via `poxconfig.cpp`, opened fresh each `reconfigure()` so a save is picked up rather than a cached copy. It resolves the `Preset-<name>` edits, `Rules`, `Active`, and `Panel` targets exactly as documented in [Configuration & Presets](configuration.gen.html); `resolve()` matches the first rule whose appId is a case-insensitive substring of the window class, with no global default. It additionally reads `kwinrc` for the configured Magic Lamp animation duration, since that non-affine warp must be covered by the un-minimize grace.

There is one wrinkle worth knowing. The bundled **KCM** (`PoxicleKcm` → `kwin_poxicle_config.so`, the "Configure" button in System Settings) presents a per-app rules table, but its `load()`/`save()` still target the legacy `kwinrc [Effect-poxicle_kwin]` group — which the runtime reader no longer consults. The supported editor is therefore the standalone [GTK4 configurator](configurator.gen.html) (`poxicle-config`), which writes the neutral file the effect actually reads; the KCM remains as the in-Settings entry point and a path for migrating older setups.

Because the plugin is loaded into the running compositor, recompiled effect or engine code does **not** hot-reload — applying it requires a real KWin restart (log out / log back in on Wayland). Palette and rule edits from the configurator *do* apply live, via the D-Bus reconfigure. During development the effect is iterated in a nested `kwin_wayland` via `run-nested.sh` (and `run-nested-chiguiro.sh`, which launches Chiguiro with `KGX_POXICLE=compositor` to drive the stream path).

## 10. Design rules

- **Compile the engine in; own the context.** KWin provides the surface and GL context, so the effect links the engine directly and feeds the renderer KWin's projection — a BYOC host.
- **Draw per window, after its content.** Per-window passes give correct occlusion by windows stacked above and let each ring be repainted independently.
- **Damage the ring, not the window.** Four thin edge strips avoid forcing a full-stack recomposite behind a translucent window — the dominant overdraw cost.
- **Park when quiet.** `m_active` and `isActive()` let KWin stop repainting, and drop the effect, when no engine is animating.
- **Restore GL state.** poxicle-gl never restores; the effect brackets every renderer call by saving and restoring program, VAO, buffers, and blend.
- **Hold through warps.** A per-window grace keyed on minimize/restore signals keeps rings from snapping in mid-animation.
