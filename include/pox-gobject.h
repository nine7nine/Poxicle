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
void           poxicle_engine_set_preset     (PoxicleEngine *self, const char *name, int reverse);
void           poxicle_engine_set_palette    (PoxicleEngine *self, int palette_id);
gboolean       poxicle_engine_apply_config   (PoxicleEngine *self, const char *wm_class);
GBytes        *poxicle_engine_tick           (PoxicleEngine *self, double dt);
GBytes        *poxicle_engine_tick_vertices  (PoxicleEngine *self, double dt);

/* Catalogue helpers (namespace-level, no engine needed) — wrap the engine's
 * canonical preset + palette tables for binding consumers. */
int            poxicle_preset_count          (void);
const char    *poxicle_preset_name           (int id);
int            poxicle_palette_count          (void);
const char    *poxicle_palette_name           (int id);

G_END_DECLS
