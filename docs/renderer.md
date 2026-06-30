# poxicle — The GLES Renderer

The renderer is `src/poxicle-gl.c` behind `include/poxicle-gl.h`: a GLES3 backend that consumes the sim's `PoxInstance[]` and draws every particle in one instanced call, with shapes evaluated as signed-distance fields in the fragment shader.

## Table of Contents

1. [What the renderer is](#1-what-the-renderer-is)
2. [One draw call for the whole frame](#2-one-draw-call-for-the-whole-frame)
3. [Instance attributes read straight from the struct](#3-instance-attributes-read-straight-from-the-struct)
4. [The vertex shader: per-particle transforms on the GPU](#4-the-vertex-shader-per-particle-transforms-on-the-gpu)
5. [The fragment shader: signed-distance shapes](#5-the-fragment-shader-signed-distance-shapes)
6. [Premultiplied blending](#6-premultiplied-blending)
7. [Two entry points: self-contained vs embedded](#7-two-entry-points-self-contained-vs-embedded)
8. [Design rules](#8-design-rules)

---

## 1. What the renderer is

`pox_gl_*` is poxicle's reference renderer: a thin GLES3 backend that turns a frame's `PoxInstance[]` into pixels. It requires only a **current OpenGL ES 3 context** — it never creates a window, a surface, or a context of its own. That is the property that makes it reusable: the same renderer runs under the [Wayland subsurface backend](wayland-backend.gen.html), a bring-your-own-context host, or a compositor effect that hands it [KWin's projection matrix](kwin-effect.gen.html).

```c
PoxGL *r = pox_gl_new();                 /* against the CURRENT GLES3 context */
/* per frame, with a current context: */
pox_gl_render(r, insts, n, width, height);
pox_gl_free(r);
```

`pox_gl_new()` compiles the shader program and sets up one VAO with two buffers — a static unit quad and a dynamic instance buffer — then returns an opaque `PoxGL`. It is deliberately small (~215 lines) because almost all the work is the GPU's.

## 2. One draw call for the whole frame

The renderer's defining choice is that **every particle in a frame is drawn by a single `glDrawArraysInstanced`**. There are no per-particle render nodes, no textures, no per-particle state changes, and no per-particle CPU transforms.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 920 300" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg    { fill: #1a1b26; }
    .box   { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .box-hot{ fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .gpu   { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .lbl   { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm{ fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut{ fill: #8c92b3; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-cy{ fill: #7dcfff; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel{ fill: #e0af68; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln    { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .title { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="920" height="300" class="bg"/>
  <text x="460" y="26" text-anchor="middle" class="title">one frame, one instanced draw</text>

  <rect x="20" y="60" width="160" height="70" class="box"/>
  <text x="100" y="88" text-anchor="middle" class="lbl-sm">sim core</text>
  <text x="100" y="106" text-anchor="middle" class="lbl-mut">pox_engine_tick</text>
  <text x="100" y="120" text-anchor="middle" class="lbl-mut">-> PoxInstance[n]</text>

  <rect x="230" y="60" width="190" height="70" class="box-hot"/>
  <text x="325" y="84" text-anchor="middle" class="lbl-yel">instance VBO</text>
  <text x="325" y="102" text-anchor="middle" class="lbl-mut">glBufferSubData: upload the</text>
  <text x="325" y="116" text-anchor="middle" class="lbl-mut">structs verbatim, no convert</text>

  <rect x="470" y="60" width="190" height="70" class="gpu"/>
  <text x="565" y="84" text-anchor="middle" class="lbl-cy">vertex shader</text>
  <text x="565" y="102" text-anchor="middle" class="lbl-mut">unit quad x instance:</text>
  <text x="565" y="116" text-anchor="middle" class="lbl-mut">rotate, scale, place, project</text>

  <rect x="710" y="60" width="190" height="70" class="gpu"/>
  <text x="805" y="84" text-anchor="middle" class="lbl-cy">fragment shader</text>
  <text x="805" y="102" text-anchor="middle" class="lbl-mut">SDF shape + fwidth AA,</text>
  <text x="805" y="116" text-anchor="middle" class="lbl-mut">premultiplied output</text>

  <line x1="180" y1="95" x2="230" y2="95" class="ln"/>
  <line x1="420" y1="95" x2="470" y2="95" class="ln"/>
  <line x1="660" y1="95" x2="710" y2="95" class="ln"/>

  <rect x="230" y="190" width="670" height="70" class="box"/>
  <text x="565" y="214" text-anchor="middle" class="lbl-yel">glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n)</text>
  <text x="565" y="234" text-anchor="middle" class="lbl-mut">4 quad vertices x n instances — the CPU issues exactly one draw per frame</text>
  <text x="565" y="250" text-anchor="middle" class="lbl-mut">no textures, no per-particle nodes, no per-particle state changes</text>
  <line x1="565" y1="130" x2="565" y2="190" class="ln"/>
</svg>
</div>

The CPU's entire per-frame job is to run the sim and upload one tight instance buffer; the GPU does every per-particle transform. This is why poxicle is fast even with hundreds of simultaneous particles — the cost scales with one buffer upload and one draw, not with particle count on the CPU side.

## 3. Instance attributes read straight from the struct

The instance buffer is the `PoxInstance[]` array itself, uploaded with no staging or conversion. The vertex array binds five **instanced** attributes (divisor 1) directly at the struct's field offsets, plus one **per-vertex** attribute (divisor 0) for the unit quad corner:

| Location | Attribute | Source | Divisor |
| --- | --- | --- | --- |
| 0 | `a_corner` (vec2) | static unit quad `[-0.5, 0.5]` | 0 (per vertex) |
| 1 | `i_pos` (vec2) | `offsetof(PoxInstance, x)` | 1 (per instance) |
| 2 | `i_size` (float) | `offsetof(PoxInstance, size)` | 1 |
| 3 | `i_angle` (float) | `offsetof(PoxInstance, angle)` | 1 |
| 4 | `i_color` (vec4) | `offsetof(PoxInstance, color)` | 1 |
| 5 | `i_shape` (int) | `offsetof(PoxInstance, shape)` | 1 |

The `shape` enum is read as a genuine integer attribute via `glVertexAttribIPointer`. Because the GL attributes alias the C struct fields, the upload is a straight `memcpy` into the VBO — the buffer grows with `glBufferData` only when a frame needs more capacity than before, and otherwise reuses the allocation with `glBufferSubData`.

## 4. The vertex shader: per-particle transforms on the GPU

Each of the four quad corners is expanded into a particle entirely in the vertex shader. It rotates the unit corner by the instance angle, scales it by `i_size`, places it at the instance's centre (`i_pos + size/2`), and projects through a single `u_mvp` matrix:

```glsl
float a = radians(i_angle);
mat2  R = mat2(cos(a), sin(a), -sin(a), cos(a));
vec2  center = i_pos + vec2(i_size * 0.5);
vec2  world  = center + R * (a_corner * i_size);
gl_Position  = u_mvp * vec4(world, 0.0, 1.0);
```

The corner position `a_corner` is passed through as `v_local` (the local `[-0.5, 0.5]` coordinate) so the fragment shader can evaluate a shape distance, and `i_color` / `i_shape` are passed through (the shape `flat`, since it must not interpolate). No CPU code ever computes a particle's screen position; the host only supplies the projection.

## 5. The fragment shader: signed-distance shapes

Shapes are **signed-distance fields** evaluated per fragment, which is what keeps them crisp at any size, scale, or rotation — there is no texture to soften at large sizes. Each shape is one closed-form distance over the local coordinate:

| `shape` | Distance field |
| --- | --- |
| `0` square | `max(|p.x|, |p.y|) - 0.5` |
| `1` circle | `length(p) - 0.5` |
| `2` diamond | `|p.x| + |p.y| - 0.5` |
| `3` triangle | `max` of three edge half-planes (`A`, `B`, `C`) |

The distance `d` is turned into coverage with analytic anti-aliasing: `aa = fwidth(d)` measures how fast the distance changes across one pixel, and `cov = 1 - smoothstep(-aa, aa, d)` gives a perfectly smooth one-pixel edge at any zoom. The output is written **premultiplied**:

```glsl
float cov = 1.0 - smoothstep(-aa, aa, d);
frag = vec4(v_color.rgb * v_color.a * cov, v_color.a * cov);
```

Because the antialiasing is derivative-based rather than a fixed texture, a 6 px firefly and a 60 px comet head are equally sharp, and a rotated triangle has no jaggies.

## 6. Premultiplied blending

The renderer sets `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` — the standard premultiplied-alpha "over" operator — to match the premultiplied fragment output above. The sim emits *straight* alpha in `PoxColor`; the fragment shader is the single place that premultiplies (`rgb * a * cov`). This pairing gives correct compositing of overlapping translucent particles and clean glow without halos, and it is the same blend math every backend reproduces: the [GObject binding](gobject-binding.gen.html) premultiplies into its Cogl vertices, and the [GNOME extension](gnome-extension.gen.html) sets the matching Cogl blend string.

The renderer **never clears** the framebuffer — clearing is the caller's responsibility. A self-contained host clears to transparent first; a bring-your-own-context host keeps its existing scene and draws particles over it.

## 7. Two entry points: self-contained vs embedded

The renderer exposes two draw functions that share one inner path; they differ only in who owns the viewport and the projection.

| Function | Owns viewport? | Projection | Used by |
| --- | --- | --- | --- |
| `pox_gl_render(r, insts, n, w, h)` | yes — sets `glViewport(0,0,w,h)` | a built-in ortho mapping top-left-origin pixels straight to clip space | the [Wayland backend](wayland-backend.gen.html), the demos, any BYOC host that thinks in surface pixels |
| `pox_gl_render_mvp(r, insts, n, mvp[16])` | no | caller-supplied column-major matrix; particle coords are interpreted in whatever space it maps from | the [KWin effect](kwin-effect.gen.html), feeding `RenderViewport::projectionMatrix()` |

The built-in ortho that `pox_gl_render` bakes in maps `(0..w, 0..h)` with y pointing down straight to clip space — exactly the convention the sim emits in. The `_mvp` variant exists for embedders that draw inside their own GL frame with their own projection, viewport, and scale handling (a compositor working in logical coordinates while the projection applies device scale). Both ultimately call the same `pox_gl_render_common`, which enables the premultiplied blend, binds the program and VAO, uploads, and issues the one draw.

## 8. Design rules

- **One draw call per frame.** All particles go through a single `glDrawArraysInstanced`; the CPU cost is one sim tick plus one buffer upload, independent of particle count.
- **The GPU does the transforms.** Rotation, scaling, placement and projection happen in the vertex shader from instance attributes; no per-particle CPU math.
- **Shapes are SDFs.** Signed-distance fields with `fwidth`-based AA stay crisp at any size and rotation — no textures to soften, no jaggies.
- **The struct is the buffer.** GL attributes alias `PoxInstance` fields, so the upload is a memcpy with no staging or conversion.
- **The caller owns context, viewport, and clear.** The renderer needs only a current GLES3 context; it never windows, and it never clears — which is exactly what lets it embed in a subsurface, a BYOC frame, or a compositor pass.
