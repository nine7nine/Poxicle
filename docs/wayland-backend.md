# poxicle — The Wayland Subsurface Backend

The Wayland backend is `src/poxicle-wl.c` behind `include/poxicle-wl.h`: an opt-in host that rides a particle overlay on a window the app already owns, using only core Wayland, with its own EGL context and a self-driving frame loop.

## Table of Contents

1. [What the backend is](#1-what-the-backend-is)
2. [Core Wayland only: the DE-agnostic subsurface](#2-core-wayland-only-the-de-agnostic-subsurface)
3. [Transparent and click-through](#3-transparent-and-click-through)
4. [Its own EGL context](#4-its-own-egl-context)
5. [The self-driving frame loop and idle parking](#5-the-self-driving-frame-loop-and-idle-parking)
6. [Positioning and resize](#6-positioning-and-resize)
7. [External render sources](#7-external-render-sources)
8. [Bring your own context](#8-bring-your-own-context)
9. [Design rules](#9-design-rules)

---

## 1. What the backend is

The reusable [core](simulation-core.gen.html) and [renderer](renderer.gen.html) need nothing but a current GL context. The Wayland backend is the first of two ways to *give* them one. It is a thin, separate unit — kept out of the core library — that creates a transparent overlay surface over a host window, equips it with an EGL/GLES3 context, and animates it independently of the host's own drawing. The host just feeds it size and triggers.

```c
PoxWl *w = pox_wl_new(display, compositor, subcompositor, parent, width, height, scale);
pox_engine_set_ambient(pox_wl_engine(w), 1);   /* drive the engine via pox_wl_engine() */
/* host commits its parent surface so the subsurface maps */
```

This is the path the Chiguiro terminal uses for its in-app overlay, and the path the `poxicle-host` demo exercises. The whole backend is ~230 lines because the heavy lifting is the core and the renderer; this file is just the Wayland and EGL plumbing.

## 2. Core Wayland only: the DE-agnostic subsurface

The overlay is a `wl_subsurface` over the host's surface, built from **only `wl_compositor` + `wl_subcompositor`** — globals that *every* Wayland compositor implements. It deliberately does **not** use `wlr-layer-shell` or any desktop-specific protocol, so the same code works on KWin, Mutter, Hyprland and wlroots alike.

A subsurface is the correct equivalent of "drawing on a window," and the reason poxicle does not try to draw directly on other apps' windows: Wayland forbids that — a client can only render to its own surfaces and can't read other windows' geometry. A subsurface instead rides *with* a host window the app already owns. The host passes the globals it already bound (display, compositor, subcompositor) plus the parent surface and current size; the backend creates the subsurface and sets it **desynchronized** (`wl_subsurface_set_desync`) so it can commit its own frames without waiting on the parent's commit cycle.

## 3. Transparent and click-through

Two properties make the overlay invisible to interaction. It is **transparent** — each frame clears to `rgba(0,0,0,0)` and draws only premultiplied particles over it — and it is **click-through**: the backend sets an **empty input region** (`wl_surface_set_input_region` with an empty `wl_region`), so pointer and touch events fall straight through to the parent window. From the user's point of view the particles decorate the window without ever intercepting a click.

## 4. Its own EGL context

The overlay gets a dedicated EGL/GLES3 context so its rendering is fully independent of how the host draws. `egl_setup()` obtains the Wayland platform display via `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, ...)`, binds the GLES API, chooses an RGBA8 config with **8-bit alpha** (required for the transparent overlay), creates a GLES3 context, wraps the subsurface in a `wl_egl_window`, and creates the window surface. After `pox_wl_new` makes the context current it constructs a [`PoxGL`](renderer.gen.html) renderer and a [`PoxEngine`](simulation-core.gen.html) sized to the window, then draws the first frame, which arms the animation loop.

## 5. The self-driving frame loop and idle parking

The backend animates itself: each frame it schedules the next via the subsurface's own `wl_surface_frame` callback, then `eglSwapBuffers` to commit. `draw()` computes `dt` from a monotonic clock (clamped to 0.1 s so a stall doesn't fast-forward the sim), ticks the engine (or an [external source](#7-external-render-sources)), clears, renders, and reschedules.

The critical refinement is **idle parking**. A desynchronized subsurface that keeps committing every vblank would keep the GPU and compositor awake forever. So once nothing is animating — `pox_engine_active()` returns false, or in source mode the source produced no instances — the loop draws a couple of trailing frames (`POX_WL_IDLE_GRACE = 2`) to flush the now-empty overlay, then stops scheduling frame callbacks and sets a `parked` flag. `pox_wl_wake()` restarts the loop when new work arrives — after firing a trigger on the engine, or when a source starts producing again. It drops the parked time gap so `dt` doesn't jump, and is cheap and idempotent while the loop is already running.

One teardown invariant earns its comment: `pox_wl_free()` must cancel any in-flight frame callback *before* tearing down its surface. Otherwise the callback fires after the free and dispatches `draw()` on freed memory — a use-after-free in `wl_surface_frame`. The free path destroys the pending callback, frees the engine, makes the context current to free the renderer, then unwinds the EGL surface, context, `wl_egl_window`, subsurface and surface in order.

## 6. Positioning and resize

`pox_wl_resize(w, width, height, scale)` keeps the overlay matched to the host window: it resizes the `wl_egl_window` and re-sizes the engine's surface so the perimeter math tracks the new geometry. `pox_wl_set_position(w, x, y)` positions the overlay within the parent in parent-local logical coordinates — used to inset past client-side-decoration shadow margins so the overlay hugs the *visible* window rather than the full surface. Position changes apply on the parent's next commit.

## 7. External render sources

The backend can draw instances from something other than its own engine. `pox_wl_set_source(w, source, user)` installs a callback that, each frame, writes up to `cap` instances into a buffer and returns the count; the overlay draws those instead of running its internal sim. Passing `NULL` reverts to the engine.

This is the in-process seam that the [poxbridge protocol](poxbridge.gen.html) generalises across processes. In source mode, "active" means "the source produced particles this frame," and the same idle-parking applies — when the source goes quiet the loop parks, and the host calls `pox_wl_wake()` when it resumes. Chiguiro uses exactly this hook to feed its own sim's `KgxParticleInstance[]` (byte-identical to `PoxInstance`) into the overlay with zero copying.

## 8. Bring your own context

The second backend isn't a file — it's the absence of one. **BYOC ("bring your own context")** means the host already has a current GL context and simply calls [`pox_gl_render(instances, ...)`](renderer.gen.html) inside its own GL frame. There is no windowing code to involve: an X11/GLX app, a game engine, or any custom GL application runs the sim with `pox_engine_tick`, then draws the result with the renderer in its existing pass. The [KWin effect](kwin-effect.gen.html) is effectively a BYOC host — it owns KWin's GL context and surface, and feeds the renderer KWin's projection matrix through `pox_gl_render_mvp`. For non-Wayland or non-cooperative cases where a subsurface isn't available, BYOC is the answer.

## 9. Design rules

- **Core Wayland only.** `wl_compositor` + `wl_subcompositor` are universal; avoiding `wlr-layer-shell` keeps the backend desktop-agnostic across KWin, Mutter, Hyprland and wlroots.
- **Ride a window you own.** Wayland forbids drawing on other clients' surfaces; a desynchronized subsurface over the host's own window is the correct, portable equivalent.
- **Invisible to input.** Transparent clear plus an empty input region make the overlay decorate without ever intercepting a click.
- **Park when quiet.** The self-driving frame loop stops committing once the engine (or source) goes idle, so the GPU and compositor sleep; `pox_wl_wake()` restarts it.
- **Two ways to supply instances.** The internal engine or an external source — and the same hook that feeds an in-process producer is what poxbridge extends across processes.
- **Cancel before free.** Destroy any pending frame callback before tearing down the surface, or it fires into freed memory.
