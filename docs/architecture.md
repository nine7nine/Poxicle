# poxicle — Architecture Overview

This page is the system map for poxicle: a toolkit- and desktop-agnostic edge-particle engine that simulates in pure C, draws in one GLES call, and rides on any surface through opt-in backends — with compositor effects and a configurator built on the same core.

## Table of Contents

1. [What poxicle is](#1-what-poxicle-is)
2. [The core idea: a current GL context is all you need](#2-the-core-idea-a-current-gl-context-is-all-you-need)
3. [Layer model](#3-layer-model)
4. [The data hand-off](#4-the-data-hand-off)
5. [Backends: giving the engine a surface](#5-backends-giving-the-engine-a-surface)
6. [Consumers in the wild](#6-consumers-in-the-wild)
7. [The one config file](#7-the-one-config-file)
8. [Source layout](#8-source-layout)
9. [Design rules](#9-design-rules)
10. [Document index](#10-document-index)

---

## 1. What poxicle is

poxicle is a small, fast particle engine that draws high-quality animated effects around the **edges of a surface** — bursts, tails, pulses, beams, ambient shimmer. It was extracted from the Chiguiro terminal's edge engine and made standalone, with its **own GPU renderer** — no GSK, no Cairo, no host toolkit involvement.

The whole engine is plain C, deliberately FFI-friendly. The reusable part needs nothing but a live OpenGL ES 3 context; everything environment-specific — how a surface or window gets created — is an opt-in backend you can ignore. That is what keeps it usable from any toolkit, any desktop, and even outside a desktop entirely (a game engine, a custom GL app). On top of that core sit three real integrations that all ride the same simulation: a [KWin Plasma 6 effect](kwin-effect.gen.html), a [GNOME Shell extension](gnome-extension.gen.html), and a [GTK4 configurator](configurator.gen.html).

<div class="diagram-container">
<svg width="100%" viewBox="0 0 980 640" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg      { fill: #1a1b26; }
    .layer-c { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .layer-r { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.5; }
    .layer-b { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.5; }
    .layer-x { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .box     { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .box-hot { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .lbl     { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-blu { fill: #7aa2f7; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy  { fill: #7dcfff; font-size: 12px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel { fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln      { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .bound   { stroke: #6b7398; stroke-width: 1.2; stroke-dasharray: 6,4; fill: none; }
    .title   { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>

  <rect x="0" y="0" width="980" height="640" class="bg"/>
  <text x="490" y="26" text-anchor="middle" class="title">poxicle layer architecture</text>

  <!-- Core -->
  <rect x="20" y="44" width="940" height="92" class="layer-c"/>
  <text x="40" y="64" class="lbl-grn">poxicle-core  --  pure-C simulation (zero deps: no GL, no Wayland, no toolkit)</text>
  <text x="40" y="78" class="lbl-mut">perimeter, envelopes, presets, palettes, bursts, geometric kinds, idle governor</text>
  <rect x="40"  y="88" width="280" height="38" class="box"/>
  <text x="180" y="111" text-anchor="middle" class="lbl-sm">pox_engine_tick(dt) -> PoxInstance[]</text>
  <rect x="332" y="88" width="300" height="38" class="box"/>
  <text x="482" y="107" text-anchor="middle" class="lbl-sm">presets + palettes (single source of truth)</text>
  <text x="482" y="120" text-anchor="middle" class="lbl-mut">pox_preset_* / pox_palette_*</text>
  <rect x="644" y="88" width="296" height="38" class="box"/>
  <text x="792" y="107" text-anchor="middle" class="lbl-sm">pox_engine_active() — idle parking</text>
  <text x="792" y="120" text-anchor="middle" class="lbl-mut">seeded clocks, edge mask, corner radius</text>

  <line x1="180" y1="136" x2="180" y2="162" class="ln"/>
  <text x="192" y="154" class="lbl-mut">PoxInstance[]</text>

  <!-- Renderer -->
  <rect x="20" y="166" width="460" height="74" class="layer-r"/>
  <text x="40" y="186" class="lbl-blu">poxicle-gl  --  GLES3 instanced / SDF renderer</text>
  <text x="40" y="200" class="lbl-mut">one glDrawArraysInstanced; shapes are signed-distance fields</text>
  <rect x="40"  y="208" width="200" height="24" class="box"/>
  <text x="140" y="225" text-anchor="middle" class="lbl-mut">pox_gl_render (owns viewport)</text>
  <rect x="252" y="208" width="208" height="24" class="box"/>
  <text x="356" y="225" text-anchor="middle" class="lbl-mut">pox_gl_render_mvp (host MVP)</text>

  <!-- Bridge -->
  <rect x="500" y="166" width="460" height="74" class="layer-x"/>
  <text x="520" y="186" class="lbl-cy">poxbridge  --  cross-process producer/receiver channel</text>
  <text x="520" y="200" class="lbl-mut">memfd shared region + seqlock; D-Bus org.ninez.PoxicleBridge</text>
  <rect x="520" y="208" width="420" height="24" class="box"/>
  <text x="730" y="225" text-anchor="middle" class="lbl-mut">a client sims + streams instances; a receiver positions + draws them</text>

  <line x1="250" y1="240" x2="250" y2="286" class="ln"/>

  <!-- Backends -->
  <rect x="20" y="290" width="940" height="86" class="layer-b"/>
  <text x="40" y="310" class="lbl-pur">backends  --  give the core a surface + a current GL context (opt-in, each a thin unit)</text>
  <rect x="40"  y="320" width="290" height="46" class="box-hot"/>
  <text x="185" y="339" text-anchor="middle" class="lbl-yel">poxicle-wl  (subsurface host)</text>
  <text x="185" y="356" text-anchor="middle" class="lbl-mut">transparent, click-through, own EGL ctx</text>
  <rect x="342" y="320" width="290" height="46" class="box"/>
  <text x="487" y="339" text-anchor="middle" class="lbl-sm">BYOC  (bring your own context)</text>
  <text x="487" y="356" text-anchor="middle" class="lbl-mut">host calls pox_gl_render in its own frame</text>
  <rect x="644" y="320" width="296" height="46" class="box"/>
  <text x="792" y="339" text-anchor="middle" class="lbl-sm">gi://Poxicle  (GObject binding)</text>
  <text x="792" y="356" text-anchor="middle" class="lbl-mut">tick_vertices -> GPU-ready blob for gjs</text>

  <line x1="20" y1="392" x2="960" y2="392" class="bound"/>
  <text x="490" y="387" text-anchor="middle" class="lbl-yel">consumers ride one of the backends above</text>

  <!-- Consumers -->
  <rect x="20" y="408" width="940" height="120" class="layer-x"/>
  <text x="40" y="428" class="lbl-cy">consumers</text>
  <rect x="40"  y="438" width="220" height="80" class="box"/>
  <text x="150" y="458" text-anchor="middle" class="lbl-sm">KWin Plasma 6 effect</text>
  <text x="150" y="476" text-anchor="middle" class="lbl-mut">BYOC: engine compiled in,</text>
  <text x="150" y="489" text-anchor="middle" class="lbl-mut">draws per window via KWin MVP,</text>
  <text x="150" y="502" text-anchor="middle" class="lbl-mut">poxbridge receiver + KCM</text>

  <rect x="272" y="438" width="220" height="80" class="box"/>
  <text x="382" y="458" text-anchor="middle" class="lbl-sm">GNOME Shell extension</text>
  <text x="382" y="476" text-anchor="middle" class="lbl-mut">gi://Poxicle in gjs,</text>
  <text x="382" y="489" text-anchor="middle" class="lbl-mut">Cogl actor draws the blob,</text>
  <text x="382" y="502" text-anchor="middle" class="lbl-mut">poxbridge receiver</text>

  <rect x="504" y="438" width="220" height="80" class="box"/>
  <text x="614" y="458" text-anchor="middle" class="lbl-sm">Chiguiro terminal</text>
  <text x="614" y="476" text-anchor="middle" class="lbl-mut">in-app poxicle-wl overlay,</text>
  <text x="614" y="489" text-anchor="middle" class="lbl-mut">or poxbridge producer</text>
  <text x="614" y="502" text-anchor="middle" class="lbl-mut">streaming to the effect</text>

  <rect x="736" y="438" width="204" height="80" class="box"/>
  <text x="838" y="458" text-anchor="middle" class="lbl-sm">GTK4 configurator</text>
  <text x="838" y="476" text-anchor="middle" class="lbl-mut">edits presets + rules,</text>
  <text x="838" y="489" text-anchor="middle" class="lbl-mut">writes the neutral config,</text>
  <text x="838" y="502" text-anchor="middle" class="lbl-mut">links engine for palettes</text>

  <!-- Config -->
  <rect x="20" y="548" width="940" height="72" class="layer-c"/>
  <text x="40" y="568" class="lbl-grn">~/.config/poxicle/poxicle.conf  --  one DE-neutral config file (GKeyFile)</text>
  <text x="40" y="582" class="lbl-mut">written by the configurator; read by the KWin effect and the GNOME extension</text>
  <rect x="40"  y="590" width="900" height="22" class="box"/>
  <text x="490" y="606" text-anchor="middle" class="lbl-mut">[poxicle]  Preset-* / Rules / Active / Panel / Corner* / UnminimizeGrace</text>
</svg>
</div>

## 2. The core idea: a current GL context is all you need

Most "draw on a window" effects reach into the host toolkit or the window manager. poxicle takes the opposite stance: the reusable engine depends on **nothing but a live OpenGL ES 3 context**. The simulation is pure C with zero dependencies; the renderer needs only a *current* context and never creates a window or surface of its own. Everything that is environment-specific — how a surface gets created, how it is positioned, how the GPU is woken — is pushed out into an opt-in backend a consumer chooses or ignores.

That single decision is what makes one engine serve a terminal overlay, a KWin compositor effect, a GNOME Shell extension, and a standalone demo. Triggers are generic too: a host fires bursts and beams through the API, and the app-specific wiring (a terminal bell, a process exit) lives in the consumer, never in the library.

## 3. Layer model

poxicle is four layers, each usable on its own:

| Layer | Code | Role |
| --- | --- | --- |
| **poxicle-core** | `src/poxicle.c`, `include/poxicle.h` | The pure-C [simulation](simulation-core.gen.html): perimeter math, envelopes, presets, palettes, burst scheduling, geometric kinds, the idle governor. Emits `PoxInstance[]`. |
| **poxicle-gl** | `src/poxicle-gl.c`, `include/poxicle-gl.h` | The [GLES3 renderer](renderer.gen.html): one instanced draw, shapes as signed-distance fields, optional embedder MVP. Needs only a current context. |
| **poxbridge** | `include/poxbridge.h` | The [cross-process channel](poxbridge.gen.html): a memfd shared region + seqlock + D-Bus, so a producer in another process can stream instances to a receiver. |
| **backends** | `poxicle-wl`, BYOC, `gi://Poxicle` | Opt-in units that [give the core a surface](wayland-backend.gen.html) and a context. |

The core and renderer build into one small static library; the Wayland backend, the GObject binding, the configurator and the demos are all separate, opt-in targets (see [Build, Install & Packaging](build.gen.html)).

## 4. The data hand-off

The seam that holds the whole system together is one flat record. The sim's only output is a `PoxInstance[]` — top-left, size, angle, shape and straight-alpha colour per particle — written by `pox_engine_tick(e, dt, out, cap)` once per frame. The sim knows nothing about how those particles are drawn or where the surface came from.

That hand-off is what lets the same frame flow three ways: a [renderer](renderer.gen.html) points GL attributes straight at the struct fields and draws all instances in one call; the [GObject binding](gobject-binding.gen.html) triangulates the instances into a GPU-ready Cogl vertex blob; and a [producer](poxbridge.gen.html) streams byte-identical records to a compositor receiver over shared memory. The record is the contract; everything else is interchangeable around it.

## 5. Backends: giving the engine a surface

A backend's only job is to hand the core a surface and a current GL context, then drive the frame loop. There are three, each a thin separate unit:

- **poxicle-wl** — a [Wayland subsurface host](wayland-backend.gen.html). It rides a transparent, click-through, desynchronized `wl_subsurface` over a window the app already owns, with its own EGL context, using only core Wayland (`wl_compositor` + `wl_subcompositor`) so it is DE-agnostic. This is the in-app overlay path.
- **BYOC** — "bring your own context." The host already has a current GL context and simply calls `pox_gl_render(instances)` inside its own frame. For X11/GLX apps, engines, or a compositor effect feeding its own projection matrix.
- **gi://Poxicle** — the [GObject binding](gobject-binding.gen.html). Not a windowing backend but the language seam: it expands a frame to a GPU-ready vertex blob a gjs consumer uploads and draws, and it owns the bridge-receiver side for GNOME.

Why not just draw on other apps' windows directly? Wayland forbids it — a client can only render to its own surfaces and can't read other windows' geometry. The subsurface is the correct equivalent (ride *with* a host window), and for non-cooperative cases BYOC or the compositor effect is the answer.

## 6. Consumers in the wild

Four programs ride this core today:

- The **[KWin Plasma 6 effect](kwin-effect.gen.html)** compiles the engine straight into a compositor plugin and draws a ring per window through KWin's projection matrix — a BYOC host that also owns the poxbridge receiver and a Configure module.
- The **[GNOME Shell extension](gnome-extension.gen.html)** drives the same engine through `gi://Poxicle` in gjs and draws the vertex blob with a Cogl actor, mirroring the effect's behaviour and owning the bridge receiver on the GNOME side.
- The **Chiguiro terminal** uses poxicle two ways: an in-app `poxicle-wl` subsurface overlay, or as a [poxbridge producer](poxbridge.gen.html) that sims in-process and streams instances to the compositor effect, so KWin does the drawing.
- The **[GTK4 configurator](configurator.gen.html)** edits the presets, per-app rules and targets, links the engine for its palette catalogue, and writes the one config file the renderers read.

## 7. The one config file

A user's choices live in a single DE-neutral file, `~/.config/poxicle/poxicle.conf`, written by the configurator and read by both compositor backends. Because the [preset and palette tables](configuration.gen.html) are engine data and the file is plain `GKeyFile`, a preset, a per-app rule, or a colour means exactly the same thing under KWin and GNOME. The renderers each resolve a window's look the same way — first matching per-app rule, else the focus-following `Active` target, with the `Panel` target for the desktop dock — so configuring once configures both.

## 8. Source layout

| Path | Purpose |
| --- | --- |
| `src/poxicle.c`, `include/poxicle.h` | The simulation core |
| `src/poxicle-gl.c`, `include/poxicle-gl.h` | The GLES3 renderer |
| `src/poxicle-wl.c`, `include/poxicle-wl.h` | The Wayland subsurface backend |
| `src/pox-gobject.c`, `include/pox-gobject.h` | The GObject / GObject-Introspection binding |
| `include/poxbridge.h` | The cross-process bridge protocol |
| `demo/main.c`, `demo/host.c` | The standalone and subsurface demos |
| `kwin/` | The KWin Plasma 6 effect + KCM (CMake/ECM) |
| `gnome/poxicle@nine7nine.github.com/` | The GNOME Shell extension |
| `configurator/` | The GTK4 configurator (Meson, opt-in) |
| `docs/` | These pages, plus the integration handoff notes |

## 9. Design rules

The architecture holds together because of a few deliberate choices:

- **The reusable core depends on nothing but a current GL context.** No toolkit, no window manager, no desktop — anything environment-specific is an opt-in backend.
- **One flat record is the contract.** The sim emits `PoxInstance[]` and nothing else; a renderer, a binding, and a cross-process stream all consume the same struct.
- **The host owns time and triggers.** The engine never reads a clock and never knows why a burst fired; `dt` and triggers come from the consumer.
- **The look is engine data.** Presets and palettes are one source of truth, so the configurator and every backend render identically.
- **One neutral config, two renderers.** A single `GKeyFile` under `~/.config/poxicle/` drives both the KWin effect and the GNOME extension.
- **Park when quiet.** `pox_engine_active()`, idle-grace frames, and a `Wake` edge let every backend sleep the GPU and the compositor when nothing animates.
- **Sim where you can, stream when you can't.** A consumer that can run the engine does; one whose triggers a compositor can't observe streams resolved instances over poxbridge instead.

## 10. Document index

| Document | Covers |
| --- | --- |
| [The Simulation Core](simulation-core.gen.html) | Perimeter math, tunables, envelopes, bursts, geometric kinds, the tick loop, seeded clocks |
| [The GLES Renderer](renderer.gen.html) | The single instanced draw, SDF shapes, the vertex/fragment shaders, premultiplied blending |
| [The poxbridge Protocol](poxbridge.gen.html) | The memfd shared region, the seqlock, the D-Bus control surface, window identity |
| [The Wayland Subsurface Backend](wayland-backend.gen.html) | The click-through subsurface, its EGL context, the self-driving loop, external sources, BYOC |
| [The GObject Binding](gobject-binding.gen.html) | `gi://Poxicle`, the two tick outputs, GPU triangulation, the config resolver, the stream receiver |
| [Configuration & Presets](configuration.gen.html) | The neutral config schema, the preset and palette catalogues, per-app rules, palette precedence |
| [The KWin Plasma 6 Effect](kwin-effect.gen.html) | The 6.7 port, the per-window paint pipeline, ring-only repaint, the grace, the bridge receiver |
| [The GNOME Shell Extension](gnome-extension.gen.html) | Driving the engine in gjs, the Cogl actor, the panel ring, minimize gating, config watching |
| [The GTK4 Configurator](configurator.gen.html) | The three pages, the rule editor, the custom cells, linking the engine for palettes |
| [Build, Install & Packaging](build.gen.html) | The Meson and CMake builds, the GIR typelib, the install scripts, the demos, sanitizers |
