/* pox-config.h
 *
 * Shared model + I/O for the standalone GTK4 Poxicle configurator. The app edits
 * the very same kwinrc [Effect-poxicle_kwin] keys the KWin effect consumes (see
 * ../src/poxconfig.{h,cpp}) — preset tunables and per-app rules — then asks KWin
 * to reload the effect. All config I/O goes through kreadconfig6/kwriteconfig6 so
 * KConfig owns the file format (no GKeyFile round-trip clobbering other effects).
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* User-tunable presets, in display order. "none" is an app state, not a tunable
 * preset, so it is not listed here. The scroll/overscroll motion has no tunable
 * preset of its own — it is driven by real scroll events, not picked as a look. */
#define POX_N_PRESETS 12
extern const char *const pox_preset_names[POX_N_PRESETS];

/* The 15 fields of a Preset-<name> entry, in serialization order. */
typedef struct {
  float speed;
  int   thickness;
  float tail_length;
  float pulse_depth;
  float pulse_speed;
  float env_attack;
  float env_release;
  int   env_curve;          /* 1 concave, 2 linear, 3 convex */
  int   release_mode;       /* PoxReleaseMode 0..4 */
  int   shape;              /* PoxShape 0..3 */
  int   gap;               /* 0 solid, 1 gapped */
  float thk_attack;
  float thk_release;
  int   thk_curve;
  int   thk_release_mode;
} PoxTune;

/* One per-app rule. Sentinels match poxconfig: -1 for the enum overrides,
 * 0 for the numeric (percent/pixel) overrides => "inherit from preset". */
typedef struct {
  char *app_id;
  char *preset;             /* preset name, or "none" to disable */
  char *color;             /* "#rrggbb" or NULL for none */
  int   reverse;           /* 0 forward, 1 reverse, 2 loop (alternate) */
  int   shape, gap, release_mode, thk_release_mode;   /* -1 = inherit */
  int   speed, thickness, tail, attack, release, thk_attack, thk_release; /* 0 = inherit */
  int   palette;           /* ambient/fireworks palette id, or -1 = use `color`; 0 = Muted */
} PoxRule;

void  pox_rule_free (PoxRule *r);

/* ---- config I/O (kwinrc [Effect-poxicle_kwin]) ---- */

void   pox_tune_seed            (const char *name, PoxTune *out);  /* built-in default */
void   pox_io_load_preset       (const char *name, PoxTune *out);  /* seed + stored override */
void   pox_io_save_preset       (const char *name, const PoxTune *t);

GPtrArray *pox_io_load_rules    (void);                  /* element-type PoxRule*, owned */
void       pox_io_save_rules    (GPtrArray *rules);

/* The focus-following "active window" target — one rule with no app id, stored in
 * the kwinrc "Active" key. The effect draws it on whichever window currently has
 * focus, on top of that window's own per-app particles. load_active always returns
 * a rule the caller frees (preset "none" => disabled). */
PoxRule   *pox_io_load_active   (void);
void       pox_io_save_active   (const PoxRule *r);

char  *pox_io_load_default_preset (void);                /* newly-allocated */
void   pox_io_save_default_preset (const char *name);

void   pox_io_reconfigure       (void);   /* DBus: KWin reload the effect, live */

/* Interactive KWin window picker (the Wayland-safe way to grab an app id — a
 * plain client can't enumerate other windows). Turns the cursor into a crosshair
 * via org.kde.KWin.queryWindowInfo; when the user clicks a window, `cb` fires with
 * its app id (resourceClass), or NULL if cancelled/unavailable. The app id is only
 * valid for the duration of `cb` — copy it to keep it. */
typedef void (*PoxPickCb) (const char *app_id, gpointer user_data);
void   pox_io_pick_window       (PoxPickCb cb, gpointer user_data);

/* ---- pages ---- */

/* Pages report a save with a short message so the window can raise a toast. */
typedef void (*PoxSavedCb) (gpointer user_data, const char *msg);

GtkWidget *pox_presets_page_new (PoxSavedCb cb, gpointer cb_data);
GtkWidget *pox_apps_page_new    (PoxSavedCb cb, gpointer cb_data);

G_END_DECLS
