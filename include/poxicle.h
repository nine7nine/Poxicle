/* poxicle.h — toolkit/DE/OS-agnostic particle edge engine (simulation core).
 *
 * Zero dependencies: no GL, no Wayland, no toolkit. The simulation advances and
 * emits a flat array of PoxInstance; a renderer backend (poxicle-gl) draws them,
 * and a windowing backend (poxicle-wl / bring-your-own-context) hosts them. This
 * header is the seam that keeps the engine reusable everywhere.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- value types (plain data, no toolkit/GL coupling) ---- */

/* Straight (non-premultiplied) RGBA, components 0..1. `a` carries the per-particle
 * alpha (tail fade, envelope). The renderer premultiplies. */
typedef struct { float r, g, b, a; } PoxColor;

typedef enum {
  POX_SHAPE_SQUARE = 0,
  POX_SHAPE_CIRCLE,
  POX_SHAPE_DIAMOND,
  POX_SHAPE_TRIANGLE,
} PoxShape;

/* One particle to draw. Produced by the sim, consumed by a renderer.
 * Coordinates are surface pixels; (x,y) is the top-left of a size×size box. */
typedef struct {
  float    x, y;
  float    size;
  float    angle;   /* degrees clockwise; only meaningful for POX_SHAPE_TRIANGLE */
  PoxShape shape;
  PoxColor color;
} PoxInstance;

typedef enum {
  POX_RELEASE_UNIFORM = 0, /* alpha fades, tail keeps length        */
  POX_RELEASE_RETRACT,     /* tail shrinks toward head              */
  POX_RELEASE_SPREAD,      /* tail blocks spread apart              */
  POX_RELEASE_GROW,        /* tail extends / thickness grows        */
  POX_RELEASE_ALL,         /* everything shrinks, head included     */
} PoxReleaseMode;

typedef enum {
  POX_EDGE_TOP = 0,
  POX_EDGE_RIGHT,
  POX_EDGE_BOTTOM,
  POX_EDGE_LEFT,
} PoxEdge;

/* High-level emission pattern for an edge stream — the per-preset identity.
 * AMBIENT/FIREWORKS use the auto-burst fan; CORNERS/PULSE_OUT/ROTATE/PING_PONG
 * run a looping geometric timeline (ported from Chiguiro's per-preset edge
 * animations); SCROLL loops an overscroll beam; NONE emits nothing continuous. */
typedef enum {
  POX_KIND_NONE = 0,
  POX_KIND_AMBIENT,
  POX_KIND_FIREWORKS,
  POX_KIND_CORNERS,
  POX_KIND_PULSE_OUT,
  POX_KIND_ROTATE,
  POX_KIND_PING_PONG,
  POX_KIND_SCROLL,
} PoxKind;

/* Tunable parameters for a particle stream (ported from the edge engine). */
typedef struct {
  float           speed;            /* speed multiplier            */
  int             thickness;        /* block size in px            */
  float           tail_length;      /* tail length multiplier      */
  float           pulse_depth;      /* shimmer intensity 0..1      */
  float           pulse_speed;      /* shimmer wave speed          */
  float           env_attack;       /* grow-in fraction 0..0.5     */
  float           env_release;      /* fade-out fraction 0..0.5    */
  int             env_curve;        /* 1 concave, 2 linear, 3 convex */
  PoxReleaseMode  release_mode;
  PoxShape        shape;
  int             gap;              /* 0 solid, 1 gapped           */
  float           thk_attack;
  float           thk_release;
  int             thk_curve;
  PoxReleaseMode  thk_release_mode;
} PoxTunables;

/* Fill `t` with sensible defaults (matches the engine's ambient look). */
void pox_tunables_default (PoxTunables *t);

/* ---- simulation engine ---- */

typedef struct PoxEngine PoxEngine;

PoxEngine *pox_engine_new  (void);
void       pox_engine_free (PoxEngine *e);

/* Surface being framed, in pixels, plus integer scale factor for HiDPI. */
void pox_engine_set_surface  (PoxEngine *e, int width, int height, int scale);

/* Tunables for the ambient/global stream. Copied; caller keeps ownership. */
void pox_engine_set_tunables (PoxEngine *e, const PoxTunables *t);

/* Ambient mode: when enabled, the engine continuously auto-fires muted-colour
 * bursts from random perimeter points (the default "alive" look). */
void pox_engine_set_ambient (PoxEngine *e, int enabled);

/* Select the emission pattern (see PoxKind). `reverse` sets travel direction for
 * the geometric kinds: 0 forward, 1 reverse, 2 loop (alternate direction on every
 * cycle). `color` tints them (a < 0 => engine default). Call AFTER
 * pox_engine_set_tunables: the geometric kinds snapshot the current tunables and
 * derive their loop duration from tunable speed. Toggles ambient mode to match
 * the kind, so callers need not manage pox_engine_set_ambient() separately. */
void pox_engine_set_kind    (PoxEngine *e, PoxKind kind, int reverse, PoxColor color);

/* Fire a burst at perimeter position `pos` (0..1 clockwise from top-left). */
void pox_engine_burst        (PoxEngine *e, float pos, PoxColor color);

/* Overscroll-style beam on an edge; progress 0..1, or <0 to clear. For hosts
 * that drive the progress themselves frame by frame. */
void pox_engine_overscroll   (PoxEngine *e, PoxEdge edge, float progress, PoxColor color);

/* Fire a self-animating overscroll beam on an edge: the engine runs the progress
 * timeline internally, so the host only triggers it once (like a tap). */
void pox_engine_fire_overscroll (PoxEngine *e, PoxEdge edge, PoxColor color);

/* Advance the sim by `dt` seconds and write up to `cap` instances into `out`.
 * Returns the count written (<= cap; excess is dropped under the draw budget).
 * This is the renderer-agnostic hand-off. */
size_t pox_engine_tick       (PoxEngine *e, double dt, PoxInstance *out, size_t cap);

/* Nonzero while anything is still animating — lets the host park its frame loop
 * (and the compositor stop waking us) when idle. */
int pox_engine_active        (const PoxEngine *e);

#ifdef __cplusplus
}
#endif
