/* pox-gobject.c — GObject/GI wrapper around the poxicle simulation core.
 * See pox-gobject.h. Keeps the wrapper thin: it owns a PoxEngine and forwards. */
#include "pox-gobject.h"
#include "poxicle.h"

#include <stdio.h>    /* sscanf for #rrggbb override colours */
#include <string.h>   /* strstr for case-folded appId substring match */

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

/* ---- config-driven look (parity with the KWin effect's poxconfig) ----------
 * The GNOME Shell extension and the KWin effect read the SAME DE-neutral file
 * (~/.config/poxicle/poxicle.conf). KWin parses it in C++/KConfig; rather than
 * maintain a second full parser in gjs, the binding resolves and applies the
 * whole look here so every backend shares one implementation. This mirrors
 * kwin/src/poxconfig.cpp field-for-field. */

#define POX_CFG_GROUP "poxicle"

/* Locale-independent float / int token parsing with a fallback. */
static double
cfg_f(const char *s, double dflt)
{
  if (!s) return dflt;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return dflt;
  gchar *end = NULL;
  double v = g_ascii_strtod(s, &end);
  return (end && end != s) ? v : dflt;
}

static int
cfg_i(const char *s, int dflt)
{
  if (!s) return dflt;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return dflt;
  gchar *end = NULL;
  gint64 v = g_ascii_strtoll(s, &end, 10);
  return (end && end != s) ? (int)v : dflt;
}

/* Parse a "#rrggbb" override colour; returns FALSE (leaving *out untouched) for
 * an empty/invalid field. */
static gboolean
cfg_color(const char *s, PoxColor *out)
{
  if (!s) return FALSE;
  while (*s == ' ' || *s == '\t') s++;
  unsigned r, g, b;
  if (*s != '#' || sscanf(s, "#%02x%02x%02x", &r, &g, &b) != 3)
    return FALSE;
  out->r = r / 255.0f;
  out->g = g / 255.0f;
  out->b = b / 255.0f;
  out->a = 1.0f;
  return TRUE;
}

/* Overlay a packed "Preset-<name>" string (15 ';'-separated fields) onto t. */
static void
cfg_parse_preset(const char *packed, PoxTunables *t)
{
  if (!packed || !*packed) return;
  gchar **f = g_strsplit(packed, ";", -1);
  if (g_strv_length(f) >= 15) {
    t->speed            = (float) cfg_f(f[0],  t->speed);
    t->thickness        =         cfg_i(f[1],  t->thickness);
    t->tail_length      = (float) cfg_f(f[2],  t->tail_length);
    t->pulse_depth      = (float) cfg_f(f[3],  t->pulse_depth);
    t->pulse_speed      = (float) cfg_f(f[4],  t->pulse_speed);
    t->env_attack       = (float) cfg_f(f[5],  t->env_attack);
    t->env_release      = (float) cfg_f(f[6],  t->env_release);
    t->env_curve        =         cfg_i(f[7],  t->env_curve);
    t->release_mode     = (PoxReleaseMode) cfg_i(f[8],  t->release_mode);
    t->shape            = (PoxShape)       cfg_i(f[9],  t->shape);
    t->gap              =         cfg_i(f[10], t->gap);
    t->thk_attack       = (float) cfg_f(f[11], t->thk_attack);
    t->thk_release      = (float) cfg_f(f[12], t->thk_release);
    t->thk_curve        =         cfg_i(f[13], t->thk_curve);
    t->thk_release_mode = (PoxReleaseMode) cfg_i(f[14], t->thk_release_mode);
  }
  g_strfreev(f);
}

/* Resolve one '|'-packed rule (preset at field `base`) and push the whole look
 * into the engine. Returns FALSE when the rule disables drawing ("none" /
 * unknown preset). Mirrors PoxConfig::resolveRule + applyPalette. */
static gboolean
cfg_apply_rule(PoxicleEngine *self, GKeyFile *kf, gchar **f, int base)
{
  guint n = g_strv_length(f);
#define TOK(i) (((guint)(base + (i)) < n && f[base + (i)]) ? f[base + (i)] : "")
  const char *preset = g_strstrip((char *) TOK(0));
  if (!*preset || g_strcmp0(preset, "none") == 0)
    return FALSE;

  PoxTunables t;
  if (!pox_preset_tunables(preset, &t))
    return FALSE;   /* unknown preset draws nothing, as in KWin */

  /* Seed from the built-in preset, then overlay stored Preset-<name> edits. */
  g_autofree char *pkey   = g_strconcat("Preset-", preset, NULL);
  g_autofree char *packed = g_key_file_get_string(kf, POX_CFG_GROUP, pkey, NULL);
  cfg_parse_preset(packed, &t);

  /* Per-app override columns (percent units; raw px for thickness). */
  int reverse = cfg_i(TOK(1), 0);
  PoxColor color = { -1.0f, -1.0f, -1.0f, -1.0f }, pc;
  if (cfg_color(TOK(2), &pc))
    color = pc;
  int shape          = cfg_i(TOK(3),  -1);
  int gap            = cfg_i(TOK(4),  -1);
  int speed          = cfg_i(TOK(5),   0);
  int thickness      = cfg_i(TOK(6),   0);
  int tail           = cfg_i(TOK(7),   0);
  int attack         = cfg_i(TOK(8),   0);
  int release        = cfg_i(TOK(9),   0);
  int releaseMode    = cfg_i(TOK(10), -1);
  int thkAttack      = cfg_i(TOK(11),  0);
  int thkRelease     = cfg_i(TOK(12),  0);
  int thkReleaseMode = cfg_i(TOK(13), -1);
  int palette        = cfg_i(TOK(14),  0);
#undef TOK

  if (shape          >= 0) t.shape = (PoxShape) shape;
  if (gap            >= 0) t.gap = gap;
  if (releaseMode    >= 0) t.release_mode = (PoxReleaseMode) releaseMode;
  if (thkReleaseMode >= 0) t.thk_release_mode = (PoxReleaseMode) thkReleaseMode;
  if (speed      != 0) t.speed = speed / 100.0f;
  if (thickness  != 0) t.thickness = thickness;
  if (tail       != 0) t.tail_length = tail / 100.0f;
  if (attack     != 0) t.env_attack = attack / 100.0f;
  if (release    != 0) t.env_release = release / 100.0f;
  if (thkAttack  != 0) t.thk_attack = thkAttack / 100.0f;
  if (thkRelease != 0) t.thk_release = thkRelease / 100.0f;

  pox_engine_set_tunables(self->engine, &t);
  pox_engine_set_kind(self->engine, pox_kind_for_preset(preset), reverse, color);
  /* Palette precedence mirrors applyPalette(): a per-app colour wins only when
   * no palette index is set (palette < 0). */
  if (palette < 0) {
    if (color.a >= 0.0f)
      pox_engine_set_palette_colors(self->engine, &color, 1);
    else
      pox_engine_set_palette(self->engine, 0);
  } else {
    pox_engine_set_palette(self->engine, palette);
  }
  return TRUE;
}

/**
 * poxicle_engine_apply_config:
 * @self: a #PoxicleEngine
 * @wm_class: (nullable): the focused window's WM class to match per-app rules
 *
 * Resolve and apply the complete look for @wm_class from poxicle-config's
 * DE-neutral config (~/.config/poxicle/poxicle.conf) — preset, its stored
 * parameter edits, the per-app override columns, palette and reverse — in one
 * call. The first per-app rule whose appId is a case-insensitive substring of
 * @wm_class wins; otherwise the focus-following "Active" target applies. This is
 * the engine-side equivalent of the KWin effect's PoxConfig, so every backend
 * honours the same edits without keeping a private parser.
 *
 * Returns: %TRUE if a look was applied, %FALSE if this window should draw
 *   nothing (no matching rule / preset "none" / no config yet with an unset
 *   target).
 */
gboolean
poxicle_engine_apply_config(PoxicleEngine *self, const char *wm_class)
{
  g_return_val_if_fail(POXICLE_IS_ENGINE(self), FALSE);

  g_autofree char *path =
      g_build_filename(g_get_user_config_dir(), "poxicle", "poxicle.conf", NULL);
  g_autoptr(GKeyFile) kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    /* No config yet: the out-of-box ambient look (matches the old JS default). */
    poxicle_engine_set_preset(self, "ambient", 0);
    pox_engine_set_palette(self->engine, 17);
    return TRUE;
  }

  /* Per-app rule: first whose appId is a case-insensitive substring of wm_class. */
  g_autofree char *rules = g_key_file_get_string(kf, POX_CFG_GROUP, "Rules", NULL);
  if (rules && wm_class && *wm_class) {
    g_autofree char *wc = g_utf8_strdown(wm_class, -1);
    gchar **lines = g_strsplit(rules, ",", -1);
    gboolean done = FALSE, applied = FALSE;
    for (int i = 0; lines[i] && !done; i++) {
      gchar **f = g_strsplit(lines[i], "|", -1);
      if (g_strv_length(f) >= 2 && f[0]) {
        g_autofree char *app = g_utf8_strdown(g_strstrip(f[0]), -1);
        if (*app && strstr(wc, app)) {
          applied = cfg_apply_rule(self, kf, f, 1);
          done = TRUE;
        }
      }
      g_strfreev(f);
    }
    g_strfreev(lines);
    if (done)
      return applied;
  }

  /* Otherwise the focus-following "Active" target, if one is configured. */
  g_autofree char *active = g_key_file_get_string(kf, POX_CFG_GROUP, "Active", NULL);
  if (active && *active) {
    gchar **f = g_strsplit(active, "|", -1);
    gboolean applied = (f[0] && *g_strstrip(f[0])) && cfg_apply_rule(self, kf, f, 0);
    g_strfreev(f);
    return applied;
  }
  return FALSE;
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
