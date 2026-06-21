/* pox-gobject.c — GObject/GI wrapper around the poxicle simulation core.
 * See pox-gobject.h. Keeps the wrapper thin: it owns a PoxEngine and forwards. */
#include "pox-gobject.h"
#include "poxicle.h"

struct _PoxicleEngine {
  GObject    parent_instance;
  PoxEngine *engine;
};

G_DEFINE_FINAL_TYPE(PoxicleEngine, poxicle_engine, G_TYPE_OBJECT)

static void
poxicle_engine_finalize(GObject *object)
{
  PoxicleEngine *self = POXICLE_ENGINE(object);
  g_clear_pointer(&self->engine, pox_engine_free);
  G_OBJECT_CLASS(poxicle_engine_parent_class)->finalize(object);
}

static void
poxicle_engine_class_init(PoxicleEngineClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = poxicle_engine_finalize;
}

static void
poxicle_engine_init(PoxicleEngine *self)
{
  self->engine = pox_engine_new();
}

/**
 * poxicle_engine_new:
 *
 * Create a new particle engine.
 *
 * Returns: (transfer full): a new #PoxicleEngine
 */
PoxicleEngine *
poxicle_engine_new(void)
{
  return g_object_new(POXICLE_TYPE_ENGINE, NULL);
}

/**
 * poxicle_engine_set_surface:
 * @self: a #PoxicleEngine
 * @width: surface width in pixels
 * @height: surface height in pixels
 * @scale: integer HiDPI scale factor (>= 1)
 *
 * Size the engine to the window it draws around.
 */
void
poxicle_engine_set_surface(PoxicleEngine *self, int width, int height, int scale)
{
  g_return_if_fail(POXICLE_IS_ENGINE(self));
  pox_engine_set_surface(self->engine, width, height, scale);
}

/**
 * poxicle_engine_set_preset:
 * @self: a #PoxicleEngine
 * @name: a built-in preset name (see poxicle_preset_name()), or "none"
 * @reverse: 0 forward, 1 reverse, 2 loop (alternate each cycle)
 *
 * Apply a preset's seed tunables and emission kind in one call.
 */
void
poxicle_engine_set_preset(PoxicleEngine *self, const char *name, int reverse)
{
  g_return_if_fail(POXICLE_IS_ENGINE(self));
  PoxTunables t;
  if (!pox_preset_tunables(name, &t))
    pox_tunables_default(&t);
  pox_engine_set_tunables(self->engine, &t);
  /* a < 0 => let the engine pick its default tone; the palette colours the rest. */
  PoxColor solid = { 0.55f, 0.78f, 1.0f, -1.0f };
  pox_engine_set_kind(self->engine, pox_kind_for_preset(name), reverse, solid);
}

/**
 * poxicle_engine_set_palette:
 * @self: a #PoxicleEngine
 * @palette_id: a built-in palette id (0 .. poxicle_palette_count()-1); < 0 leaves
 *   the current palette unchanged
 *
 * Choose the built-in colour palette every emission kind samples.
 */
void
poxicle_engine_set_palette(PoxicleEngine *self, int palette_id)
{
  g_return_if_fail(POXICLE_IS_ENGINE(self));
  if (palette_id >= 0)
    pox_engine_set_palette(self->engine, palette_id);
}

/**
 * poxicle_engine_tick:
 * @self: a #PoxicleEngine
 * @dt: seconds elapsed since the previous tick
 *
 * Advance the simulation by @dt and return this frame's particle instances as a
 * packed blob. Each instance is 36 bytes, little-endian, in this field order:
 * float32 x, y, size, angle; int32 shape (0 square, 1 circle, 2 diamond,
 * 3 triangle); float32 r, g, b, a. The number of instances is the blob size / 36.
 *
 * Returns: (transfer full): the packed #PoxInstance array for this frame
 */
GBytes *
poxicle_engine_tick(PoxicleEngine *self, double dt)
{
  g_return_val_if_fail(POXICLE_IS_ENGINE(self), NULL);
  const size_t cap = 4096;
  PoxInstance *buf = g_new(PoxInstance, cap);
  size_t n = pox_engine_tick(self->engine, dt, buf, cap);
  GBytes *bytes = g_bytes_new(buf, n * sizeof(PoxInstance));
  g_free(buf);
  return bytes;
}

/**
 * poxicle_preset_count:
 *
 * Returns: the number of built-in presets.
 */
int poxicle_preset_count(void) { return pox_preset_count(); }

/**
 * poxicle_preset_name:
 * @id: preset index, 0 .. poxicle_preset_count()-1
 *
 * Returns: (nullable): the preset's name, or %NULL if out of range
 */
const char *poxicle_preset_name(int id) { return pox_preset_name(id); }

/**
 * poxicle_palette_count:
 *
 * Returns: the number of built-in palettes.
 */
int poxicle_palette_count(void) { return pox_palette_count(); }

/**
 * poxicle_palette_name:
 * @id: palette index, 0 .. poxicle_palette_count()-1
 *
 * Returns: (nullable): the palette's name, or %NULL if out of range
 */
const char *poxicle_palette_name(int id) { return pox_palette_name(id); }
