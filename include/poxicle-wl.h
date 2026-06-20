/* poxicle-wl.h — Wayland subsurface host backend.
 *
 * Rides a particle overlay *on a window the host app already owns*, using only
 * core Wayland (wl_compositor + wl_subcompositor — no wlr-layer-shell, so it
 * works on KWin/Mutter/Hyprland/wlroots alike). It creates a transparent,
 * click-through (empty input region), desynchronized wl_subsurface over the
 * host's surface, gives it its own EGL/GLES3 context, and self-animates via the
 * subsurface's frame callbacks. The host just feeds it size + triggers.
 */
#pragma once

#include <wayland-client.h>

#include "poxicle.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PoxWl PoxWl;

/* Create the overlay over `parent`. The host passes the globals it already bound
 * (display + compositor + subcompositor) and the current window size/scale. The
 * parent must be committed by the host afterwards so the subsurface maps.
 * Returns NULL on failure. */
PoxWl *pox_wl_new (struct wl_display       *display,
                   struct wl_compositor    *compositor,
                   struct wl_subcompositor *subcompositor,
                   struct wl_surface       *parent,
                   int width, int height, int scale);

void pox_wl_free   (PoxWl *w);

/* Keep the overlay matched to the host window on resize. */
void pox_wl_resize (PoxWl *w, int width, int height, int scale);

/* Restart the overlay's frame loop after it has parked itself for being idle.
 * The loop self-parks when nothing is animating (so the GPU/compositor sleep);
 * call this when new work arrives — after firing a trigger on the engine, or
 * (in source mode) when the source starts producing instances again. Idempotent
 * and cheap while the loop is already running. */
void pox_wl_wake (PoxWl *w);

/* Position the overlay within the parent surface, in parent-local (logical)
 * coordinates. Use this to inset past client-side-decoration shadow margins so
 * the overlay hugs the visible window rather than the full surface. Applies on
 * the parent's next commit. */
void pox_wl_set_position (PoxWl *w, int x, int y);

/* The simulation engine, for firing triggers (bursts / overscroll / ambient).
 * See poxicle.h. */
PoxEngine *pox_wl_engine (PoxWl *w);

/* Optional external render source. When set, each frame the overlay draws the
 * instances this callback writes instead of running its own sim engine — use it
 * to render another engine's per-frame output (e.g. for a renderer comparison).
 * The callback writes up to `cap` instances into `out` and returns the count.
 * Pass NULL to revert to the internal engine. */
typedef size_t (*PoxWlSource) (PoxInstance *out, size_t cap, void *user);
void pox_wl_set_source (PoxWl *w, PoxWlSource source, void *user);

#ifdef __cplusplus
}
#endif
