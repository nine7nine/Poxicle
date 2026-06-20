# poxicle

A small, **toolkit- and desktop-agnostic** particle engine that draws fast,
high-quality animated effects around the **edges of a surface** — bursts, tails,
pulses, beams, ambient shimmer. Extracted from the Chiguiro terminal's edge engine
and made standalone, with **its own GPU renderer** (no GSK, no Cairo, no host
toolkit involvement).

## Design principle

The reusable core needs **nothing but a live OpenGL ES 3 context**. Anything
environment-specific (how a surface/window gets created) is an **opt-in backend**
you can ignore. That keeps the engine usable from any toolkit, any desktop, and
even outside a desktop entirely (a game engine, a custom GL app).

## Architecture

```
poxicle-core  Pure C simulation: perimeter math, envelopes, tunables, burst
              scheduling, adaptive frame governor. No GL, no Wayland, no toolkit.
              Emits a per-frame array of PoxInstance {x,y,size,angle,shape,rgba}.
              -> include/poxicle.h

poxicle-gl    GLES3 renderer. Consumes PoxInstance[], issues ONE instanced draw
              call; shapes are signed-distance fields in the fragment shader.
              Needs only a *current* GL context — no windowing. Optional bloom
              pass for glow.
              -> include/poxicle-gl.h

backends (opt-in, each a thin separate unit):
  poxicle-wl    Core-Wayland host: a transparent, click-through (empty input
                region), desynchronized wl_subsurface over a host window, with its
                own EGL context. Uses only wl_compositor + wl_subcompositor, which
                every Wayland compositor implements (KWin, Mutter, Hyprland,
                wlroots) — no wlr-layer-shell, so it is DE-agnostic.
  poxicle-byoc  "Bring your own context": the host calls pox_gl_render(instances)
                inside its own GL frame. For X11/GLX apps, engines, anything.
```

Triggers are generic: the host fires bursts via the API. Terminal/app-specific
wiring lives in the *consumer*, never in the library.

## Fast AF

- All particles in a **single `glDrawArraysInstanced`** — one draw call per frame.
- The **GPU** does every per-particle transform in the vertex shader from instance
  attributes; the CPU only runs the sim and uploads one tight instance buffer.
- No textures, no per-particle render nodes, no per-particle state changes.

## High quality

- Shapes are **signed-distance fields** → analytic anti-aliasing (smoothstep over
  the distance), perfectly crisp at any size, scale, or rotation. No texture
  softening at large sizes.
- Premultiplied-alpha / linear blending; optional **bloom** for genuine glow.

## Why not draw directly on other apps' windows?

Wayland forbids it (a client can only render to its own surfaces and can't read
other windows' geometry). The `poxicle-wl` backend's subsurface is the correct
equivalent: it rides *with* a host window the app already owns. For non-Wayland or
non-cooperative cases, use `poxicle-byoc`.

## Status

- [x] Phase 0: scaffold + toolkit-free core API header
- [x] Phase 1: GLES3 instanced/SDF renderer + standalone demo (speed/quality proven)
- [x] Phase 2: backends — `poxicle-byoc` and `poxicle-wl` subsurface host
- [x] Phase 3: generic triggers + real consumers (Chiguiro terminal; KWin Plasma 6 effect)

Consumers in the wild: the **Chiguiro** terminal (in-app subsurface overlay, and
rendering through the compositor) and a standalone **KWin Plasma 6** edge-particle
effect — both ride this same core.

## Toolchain (verified here)

wayland-client 1.25, wayland-egl 18.1, EGL 1.5, GLES 3.2 (glesv2), wayland-scanner,
meson, Mesa on Intel Iris Plus. Language: C (FFI-friendly for any consumer).

## Build

```bash
meson setup build
meson compile -C build
```

### Sanitizers (ASAN/UBSAN)

The library is meant to be developed and tested under AddressSanitizer +
UndefinedBehaviorSanitizer. Consumers (e.g. Chiguiro) may link poxicle
statically into a non-sanitized binary, so the sanitizer coverage lives here, in
poxicle's own build, where the library and its demos link the sanitizer runtimes
directly:

```bash
meson setup build-asan -Db_sanitize=address,undefined
meson compile -C build-asan

# Run a demo with errors made fatal. lsan.supp filters out Mesa's EGL/GL driver
# leaks (process-lifetime driver state, not poxicle's) so real leaks surface:
LSAN_OPTIONS=suppressions=lsan.supp \
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  build-asan/poxicle-host
```

`poxicle-host` exercises the `poxicle-wl` subsurface backend (the path Chiguiro
uses); `poxicle-demo` exercises the standalone core + GLES renderer.

> Lineage: a clean reimplementation of the math in Chiguiro's `kgx-edge*.c` /
> `kgx-particle.c`.

## License

GNU Lesser General Public License v3.0 or later (`LGPL-3.0-or-later`). See
[`COPYING.LESSER`](COPYING.LESSER) (the LGPL terms) and [`COPYING`](COPYING) (the
GPL terms it builds on). The weak copyleft is deliberate: the engine can be linked
into consumers under any license — terminals, compositor effects, games — while
improvements to the engine itself stay open.
