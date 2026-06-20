/* poxicle-gl.h — GLES3 instanced/SDF renderer backend.
 *
 * Consumes PoxInstance[] from the sim and issues a single instanced draw with
 * shapes as signed-distance fields. Requires a *current* OpenGL ES 3 context —
 * the caller owns the context/surface, so this works under the wl subsurface
 * host, a bring-your-own-context host, or (later) a compositor effect.
 */
#pragma once

#include <stddef.h>

#include "poxicle.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PoxGL PoxGL;

/* Create the renderer against the CURRENT GLES3 context. NULL on failure. */
PoxGL *pox_gl_new  (void);
void   pox_gl_free (PoxGL *r);

/* Draw `n` instances into the current framebuffer, sized width×height px. Sets
 * the viewport and premultiplied-alpha blend, then issues one
 * glDrawArraysInstanced. Does NOT clear — the caller owns clearing, so a
 * bring-your-own-context host keeps its existing scene. */
void   pox_gl_render (PoxGL *r, const PoxInstance *insts, size_t n,
                      int width, int height);

/* As pox_gl_render, but the caller supplies the projection matrix (column-major,
 * 16 floats) and owns the viewport — for embedders that draw inside their own GL
 * frame with their own projection (e.g. a compositor effect feeding KWin's
 * RenderViewport::projectionMatrix()). Particle coordinates are interpreted in
 * the space that matrix maps from. Does NOT set the viewport or clear. */
void   pox_gl_render_mvp (PoxGL *r, const PoxInstance *insts, size_t n,
                          const float mvp[16]);

#ifdef __cplusplus
}
#endif
