/* pox-gobject.h — GObject/GObject-Introspection wrapper around the poxicle
 * simulation core (poxicle.h). This is the seam that lets language bindings —
 * notably the GNOME Shell extension (gjs) — drive the SAME C engine the KWin
 * effect links, instead of maintaining a separate port. The core engine stays
 * dependency-free; this wrapper adds the GObject layer on top.
 *
 * Introspected as the "Poxicle" namespace: in gjs, `import Pox from 'gi://Poxicle'`
 * then `new Pox.Engine()`.
 */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define POXICLE_TYPE_ENGINE (poxicle_engine_get_type())
G_DECLARE_FINAL_TYPE(PoxicleEngine, poxicle_engine, POXICLE, ENGINE, GObject)

PoxicleEngine *poxicle_engine_new            (void);
void           poxicle_engine_set_surface    (PoxicleEngine *self, int width, int height, int scale);
void           poxicle_engine_set_corner_radius (PoxicleEngine *self, int top, int bottom);
void           poxicle_engine_set_seed       (PoxicleEngine *self, guint seed);
void           poxicle_engine_set_preset     (PoxicleEngine *self, const char *name, int reverse);
void           poxicle_engine_set_palette    (PoxicleEngine *self, int palette_id);
gboolean       poxicle_engine_apply_config   (PoxicleEngine *self, const char *wm_class);
GBytes        *poxicle_engine_tick           (PoxicleEngine *self, double dt);
GBytes        *poxicle_engine_tick_vertices  (PoxicleEngine *self, double dt);

/* External-stream (bridge receiver) API. A producer process (e.g. Chiguiro) runs
 * its own sim and streams ready-to-draw instances over a shared memfd; this side
 * maps that region read-only and converts each new frame to the same GPU-ready
 * vertex blob tick_vertices() yields — no local sim. Mirrors the KWin effect's
 * ExtStream, so the GNOME Shell extension can own org.ninez.PoxicleBridge too. */
gboolean       poxicle_engine_attach_stream        (PoxicleEngine *self, int fd);
GBytes        *poxicle_engine_read_stream_vertices (PoxicleEngine *self);
void           poxicle_engine_detach_stream        (PoxicleEngine *self);
int            poxicle_engine_stream_width         (PoxicleEngine *self);
int            poxicle_engine_stream_height        (PoxicleEngine *self);

/* Catalogue helpers (namespace-level, no engine needed) — wrap the engine's
 * canonical preset + palette tables for binding consumers. */
int            poxicle_preset_count          (void);
const char    *poxicle_preset_name           (int id);
int            poxicle_palette_count          (void);
const char    *poxicle_palette_name           (int id);

G_END_DECLS
