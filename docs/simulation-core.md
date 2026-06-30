# poxicle — The Simulation Core

The simulation core is `src/poxicle.c` behind `include/poxicle.h`: a toolkit-, GL- and Wayland-free engine that advances a particle stream around the edges of a surface and emits a flat `PoxInstance[]` for any renderer to draw.

## Table of Contents

1. [What the core is](#1-what-the-core-is)
2. [Value types: the renderer-agnostic hand-off](#2-value-types-the-renderer-agnostic-hand-off)
3. [The surface and its perimeter](#3-the-surface-and-its-perimeter)
4. [Tunables and envelopes](#4-tunables-and-envelopes)
5. [Emission models: bursts, geometric kinds, overscroll](#5-emission-models-bursts-geometric-kinds-overscroll)
6. [The tick loop](#6-the-tick-loop)
7. [Per-engine clocks and idle parking](#7-per-engine-clocks-and-idle-parking)
8. [Design rules](#8-design-rules)

---

## 1. What the core is

poxicle's core is a **pure-C simulation** with zero dependencies — no GL, no Wayland, no toolkit, not even glib. It is a clean reimplementation of the math in the Chiguiro terminal's edge engine (`kgx-edge-draw.c` / `kgx-edge-burst.c` / `kgx-particle.c`), collapsed to a **single full-surface emitter**: there is no per-side widget split, so every block is emitted directly into one flat array.

The contract is small. A host sizes the engine to a surface, selects a look, and calls `pox_engine_tick(e, dt, out, cap)` once per frame; the engine writes that frame's particles into `out` and returns the count. Everything downstream — uploading the array, drawing it, hosting a window — is a separate concern handled by a [renderer](renderer.gen.html) and a [backend](wayland-backend.gen.html). Time is supplied entirely by the host as `dt` (the frame delta in seconds); all scheduling inside the engine is in engine-local seconds, so the core never reads a clock.

```c
PoxEngine *e = pox_engine_new();
pox_engine_set_surface(e, w, h, scale);
pox_engine_set_preset_look(e);          /* tunables + kind + palette */

PoxInstance buf[4096];
size_t n = pox_engine_tick(e, dt, buf, 4096);
/* hand buf[0..n) to a renderer */
```

The whole public surface is the `pox_engine_*` family in `poxicle.h`; the `PoxEngine` struct itself is opaque.

## 2. Value types: the renderer-agnostic hand-off

Every type that crosses the engine boundary is plain data with no toolkit or GL coupling, so the same records feed a GLES renderer, a Cogl pipeline, or a cross-process stream unchanged.

| Type | Shape | Notes |
| --- | --- | --- |
| `PoxColor` | `float r, g, b, a` | **Straight** (non-premultiplied) RGBA, 0..1. `a` carries the per-particle alpha (tail fade, envelope). The renderer premultiplies. |
| `PoxShape` | enum | `SQUARE`, `CIRCLE`, `DIAMOND`, `TRIANGLE`. |
| `PoxInstance` | `x, y, size, angle, shape, color` | One particle. `(x, y)` is the **top-left** of a `size × size` box in surface pixels; `angle` is degrees clockwise and only meaningful for `TRIANGLE`. |

`PoxInstance` is the single hand-off record. It is laid out so a GL backend can point vertex attributes straight at the struct fields (see [The GLES Renderer](renderer.gen.html)) and so a producer in another process can stream byte-identical records (see [The poxbridge Protocol](poxbridge.gen.html)). At roughly 36 bytes per instance, a full frame of a few hundred particles is a few kilobytes.

## 3. The surface and its perimeter

The engine frames a rectangle. `pox_engine_set_surface(e, width, height, scale)` records the pixel size and an integer HiDPI scale, and computes the **perimeter** `perim = 2·(width + height)`. Every emission is expressed as a position along that perimeter.

A perimeter position `d` runs in surface pixels, clockwise from the top-left corner. `block_at()` maps `d` to a block's top-left `(px, py)` hugging the inside edge, plus the facing angle a triangle should point:

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 400" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg    { fill: #1a1b26; }
    .surf  { fill: #1f2535; stroke: #565f89; stroke-width: 1; }
    .edge  { fill: #24283b; stroke: #7aa2f7; stroke-width: 1.5; }
    .lbl   { fill: #c0caf5; font-size: 12px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm{ fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut{ fill: #8c92b3; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy{ fill: #7dcfff; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel{ fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .title { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .corner{ fill: #bb9af7; }
  </style>
  <rect x="0" y="0" width="900" height="400" class="bg"/>
  <text x="450" y="28" text-anchor="middle" class="title">perimeter parameterization (clockwise from top-left)</text>

  <rect x="300" y="70" width="300" height="240" class="surf"/>
  <rect x="300" y="70" width="300" height="16" class="edge"/>
  <rect x="584" y="70" width="16" height="240" class="edge"/>
  <rect x="300" y="294" width="300" height="16" class="edge"/>
  <rect x="300" y="70" width="16" height="240" class="edge"/>

  <circle cx="300" cy="70" r="5" class="corner"/>
  <circle cx="600" cy="70" r="5" class="corner"/>
  <circle cx="600" cy="310" r="5" class="corner"/>
  <circle cx="300" cy="310" r="5" class="corner"/>

  <text x="296" y="64" text-anchor="end" class="lbl-yel">d = 0</text>
  <text x="450" y="62" text-anchor="middle" class="lbl-cy">top:    0 .. w</text>
  <text x="612" y="195" class="lbl-cy">right:  w .. w+h</text>
  <text x="450" y="332" text-anchor="middle" class="lbl-cy">bottom: w+h .. 2w+h</text>
  <text x="288" y="195" text-anchor="end" class="lbl-cy">left: 2w+h .. 2(w+h)</text>

  <text x="450" y="200" text-anchor="middle" class="lbl-mut">w x h surface</text>
  <text x="450" y="218" text-anchor="middle" class="lbl-mut">perim = 2(w + h)</text>

  <text x="40"  y="360" class="lbl-sm">block_at(w, h, d) -> top-left (px, py) + triangle facing angle</text>
  <text x="40"  y="380" class="lbl-mut">each block is clamped fully inside the rect, so a corner turn never pokes out</text>
</svg>
</div>

Two surface modifiers reshape the ring without changing this scheme:

- **Corner rounding** — `pox_engine_set_corner_radius(e, top, bottom)` rounds the two top corners by `top` pixels and the two bottom corners by `bottom`, so a host can match KDE (top corners only) or GNOME (all four). Within a radius of a corner, `block_at_rounded()` follows a **quadratic Bézier** from the straight run, through the sharp corner point, to the next straight run. The curve's endpoints coincide with the square mapping, so the straight edges are untouched and there is no seam. Each radius is capped at half the shorter side. `0` (the default) is bit-for-bit the historical square path.
- **Edge mask** — `pox_engine_set_edge_mask(e, top, right, bottom, left)` restricts the ring to a subset of edges; each block is classified from its raw (pre-rounding) position and dropped if it lands on a disabled edge. The default `0xF` (all four) costs nothing. This exists for surfaces docked to a screen border — a desktop panel — where the edges flush against the border should carry no particles and only the interior-facing edge(s) get the ring. The host decides which edges face the interior.

## 4. Tunables and envelopes

A look is a `PoxTunables` struct, ported field-for-field from the edge engine. It governs the geometry and timing of a stream:

| Field | Meaning |
| --- | --- |
| `speed` | Speed multiplier; also scales loop/burst durations. |
| `thickness` | Block size in pixels. |
| `tail_length` | Tail length multiplier (over a 200 px base segment). |
| `pulse_depth` / `pulse_speed` | Shimmer intensity (0..1) and wave speed along a tail. |
| `env_attack` / `env_release` | Grow-in and fade-out fractions (each 0..0.5) of the life cycle. |
| `env_curve` | `1` concave (√), `2` linear, `3` convex (²). |
| `release_mode` | `UNIFORM`, `RETRACT`, `SPREAD`, `GROW`, `ALL` — how the tail dissolves. |
| `shape` | The `PoxShape` drawn. |
| `gap` | `0` solid run, `1` gapped into distinct particles. |
| `thk_attack` / `thk_release` / `thk_curve` / `thk_release_mode` | A second envelope on **thickness**, independent of the alpha envelope. |

`pox_tunables_default()` fills sensible defaults — `speed 1.0`, `thickness 10`, triangle shape, a 0.12/0.20 attack/release — matching the engine's ambient look. `pox_envelope(t, attack, release, curve)` evaluates the shared attack/sustain/release curve used by every emission path, so alpha, tail length, thickness and spread all breathe through one function.

A host rarely fills tunables by hand. The **preset catalogue** is the single source of truth for seed tunables and the name→emission-kind mapping; see [Configuration & Presets](configuration.gen.html) for the full list and the `pox_preset_*` / `pox_palette_*` lookups.

## 5. Emission models: bursts, geometric kinds, overscroll

The `PoxKind` selected by `pox_engine_set_kind(e, kind, reverse, color)` chooses one of three internal emission machines. Selecting a kind also toggles ambient mode to match, so callers need not manage it separately.

**Bursts (the auto-fire fan).** `AMBIENT` and `FIREWORKS` continuously auto-fire bursts from random perimeter points — the default "alive" look. Each `PoxBurst` is a short-lived event: two trailing segments spread out from a head point and fade over an ease-out-cubic timeline (`0.8 / speed` seconds). `ambient_schedule()` staggers the fan with randomized delays scaled by speed; `FIREWORKS` runs a denser, tighter six-slot fan. When the engine is reversed, the two segments *implode* — they start spread apart and converge to the head, shrinking as they meet. A host can also fire a one-off burst at any perimeter position with `pox_engine_burst(e, pos, color)`, which uses the manual slots above the ambient fan (the pool holds `POX_MAX_BURSTS = 8` total).

**Geometric kinds (the looping timeline).** The eighteen geometric presets — `CORNERS`, `PULSE_OUT`, `ROTATE`, `PING_PONG`, `LASER`, `TRACER`, `COMET`, `SPINNER`, `RIPPLE`, `CHARGE`, `SPREAD`, `RADAR`, `COUNTERSPIN`, `SNAKE`, `BREATHE`, `STROBE`, `FIREFLIES` — run a single looping `proc_*` timeline. Each snapshots the current tunables at `set_kind` time and derives its loop duration from tunable speed (`proc_duration_ms`, e.g. `ROTATE` 3.5 s, `LASER` 1.4 s, `COMET` 6 s). `tick()` re-arms the timeline on completion so the motion is continuous, stepping a per-cycle palette colour each lap. The `reverse` argument is tri-state: `0` forward, `1` reverse, `2` loop (alternate direction every cycle). Constant-speed motions run linear; the rest ease-out-cubic. Kinds like `BREATHE`, `STROBE` and the `RADAR` backdrop use `emit_ring()` — a uniform-alpha ring of single-block segments around the whole perimeter.

**Overscroll beams.** A host can drive an edge "overscroll" beam — a corner burst running along two adjacent edges — either frame-by-frame with `pox_engine_overscroll(e, edge, progress, color)`, or fire-and-forget with `pox_engine_fire_overscroll()`, which runs the progress timeline internally over 0.6 s. The `SCROLL` kind simply keeps re-firing this beam in a loop, stepping the palette each time.

Across all three machines, colour comes from the active **palette**: ambient/fireworks sample a fresh colour per burst, geometric kinds sample deterministically per segment index (multi-arm kinds) or per loop cycle (single-arm kinds), so the look is stable frame-to-frame rather than strobing. A palette is chosen with `pox_engine_set_palette(e, id)` or supplied directly with `pox_engine_set_palette_colors()`.

## 6. The tick loop

`pox_engine_tick(e, dt, out, cap)` is the whole per-frame engine. In order, it:

1. Advances engine-local time by `dt`.
2. Fires any scheduled ambient work whose due time has passed, and reschedules the fan.
3. Advances and emits every active burst (the full 8-slot pool).
4. Advances the geometric-kind timeline, re-arming it (and stepping the palette / flipping direction) when a loop completes, then emits the current kind.
5. Re-fires the `SCROLL` beam if that kind is active and idle.
6. Advances any self-animating overscroll timeline and emits the beam.

It returns the number of instances written, never more than `cap`; excess is simply dropped under the draw budget. The emit helpers (`emit_segment`, `emit_burst`, `emit_kind`, `emit_ring`, `emit_overscroll`) all funnel through `emit_segment`, which walks a trailing run of blocks, applies the alpha and thickness envelopes, the per-block pulse shimmer, the gap/spread spacing, and the edge-mask cull, then writes each block's `PoxInstance`. The engine carries its own tiny xorshift PRNG so it needs no external randomness source.

## 7. Per-engine clocks and idle parking

Two properties make the core safe to run in many independent instances and cheap when nothing moves.

**Independent clocks.** `pox_engine_set_seed(e, seed)` gives an engine its own random phase and RNG stream. The seed is mixed internally (so a simple per-object counter is fine), and the resulting phase back-dates loop starts and staggers the first auto-burst. Two engines created in the same frame — say a desktop panel ring and the active-window ring — then animate on their own clocks instead of marching in lockstep. Not calling it leaves the engine on the legacy fixed seed and zero phase, unchanged. Every integration ([KWin](kwin-effect.gen.html), [GNOME](gnome-extension.gen.html)) seeds each engine from a monotonic counter for exactly this reason.

**Idle parking.** `pox_engine_active(e)` returns nonzero while *anything* is still animating — an active burst, a looping geometric kind, a live overscroll, or pending ambient work. A host uses this to park its frame loop (and let the compositor stop waking it) when the ring goes quiet, and to restart only when new work arrives. This single predicate is what lets both the Wayland backend and the compositor effects sleep the GPU between flourishes.

## 8. Design rules

- **Zero dependencies in the core.** No GL, Wayland, toolkit, or glib — the simulation is portable C and embeddable anywhere a host can call a function once per frame.
- **The host owns time.** The engine never reads a clock; `dt` is supplied. That keeps it deterministic, testable, and trivially driveable from a compositor's present timestamps or a frame callback.
- **One flat array out.** The sim's only output is `PoxInstance[]`. It knows nothing about how those particles are drawn or hosted, which is exactly why the same core drives a GLES renderer, a Cogl pipeline, and a cross-process stream.
- **Every look reduces to data.** A preset is tunables + a kind + a palette — all plain values. There is no per-look code path outside the engine, so the configurator and every backend share one implementation.
- **Independent and idle-aware.** Seeded clocks let many rings coexist without visual lockstep; `pox_engine_active()` lets the host sleep when idle.
