/* pox-io.c — kwinrc [Effect-poxicle_kwin] I/O. See pox-config.h. */
#include "pox-config.h"

#include <gio/gio.h>
#include <stdlib.h>

const char *const pox_preset_names[POX_N_PRESETS] = {
  "ambient", "corners", "fireworks", "ping-pong", "pulse-out", "rotate",
  "laser", "tracer", "comet", "spinner", "ripple", "charge",
};

/* Built-in seeds — a verbatim copy of poxconfig.cpp kSeeds (the effect's
 * defaults). Order: speed, thickness, tail, pulseDepth, pulseSpeed, envAtk,
 * envRel, envCurve, releaseMode, shape, gap, thkAtk, thkRel, thkCurve, thkRelMode. */
typedef struct { const char *name; PoxTune t; } Seed;
static const Seed kSeeds[] = {
  { "none",      { 0.7f,  6, 0.7f, 0.0f,  0.3f, 0.2f, 0.05f, 2, 1, 3, 0, 0.0f,  0.0f, 2, 0 } },
  { "ambient",   { 0.3f, 30, 0.9f, 1.0f,  2.1f, 0.2f, 0.5f,  1, 0, 0, 0, 0.5f,  0.0f, 3, 0 } },
  { "corners",   { 1.1f, 20, 0.9f, 0.5f,  0.8f, 0.2f, 0.3f,  2, 0, 0, 0, 0.0f,  0.0f, 2, 0 } },
  { "fireworks", { 1.4f, 20, 0.9f, 0.65f, 0.9f, 0.2f, 0.3f,  2, 1, 0, 0, 0.4f,  0.0f, 2, 0 } },
  { "ping-pong", { 0.5f, 20, 0.1f, 0.45f, 0.8f, 0.0f, 0.0f,  2, 0, 1, 0, 0.0f,  0.0f, 2, 0 } },
  { "pulse-out", { 1.0f, 20, 0.9f, 0.5f,  0.8f, 0.0f, 0.1f,  2, 1, 0, 0, 0.5f,  0.0f, 2, 0 } },
  { "rotate",    { 1.5f, 20, 0.9f, 0.5f,  0.8f, 0.2f, 0.3f,  2, 0, 0, 0, 0.15f, 0.3f, 2, 0 } },
  { "laser",     { 2.5f,  8, 0.4f, 0.0f,  0.5f, 0.0f, 0.1f,  2, 0, 0, 1, 0.0f,  0.0f, 2, 0 } },
  { "tracer",    { 1.2f, 12, 0.5f, 0.2f,  0.8f, 0.1f, 0.2f,  2, 0, 0, 0, 0.0f,  0.0f, 2, 0 } },
  { "comet",     { 0.8f, 16, 1.5f, 0.3f,  0.8f, 0.1f, 0.2f,  2, 0, 0, 1, 0.0f,  0.0f, 2, 0 } },
  { "spinner",   { 1.3f, 14, 0.9f, 0.2f,  0.8f, 0.1f, 0.2f,  2, 0, 0, 1, 0.0f,  0.0f, 2, 0 } },
  { "ripple",    { 1.0f, 14, 0.6f, 0.3f,  0.8f, 0.15f,0.3f,  2, 0, 1, 1, 0.0f,  0.0f, 2, 0 } },
  { "charge",    { 1.0f, 16, 0.6f, 0.3f,  0.8f, 0.15f,0.3f,  2, 0, 1, 1, 0.0f,  0.0f, 2, 0 } },
};

/* ---- KConfig CLI helpers (KConfig owns the file format) ---- */

static char *read_key(const char *key)
{
  GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL,
                                    "kreadconfig6", "--file", "kwinrc",
                                    "--group", "Effect-poxicle_kwin",
                                    "--key", key, NULL);
  if (!p)
    return g_strdup("");

  char *out = NULL;
  if (!g_subprocess_communicate_utf8(p, NULL, NULL, &out, NULL, NULL) || !out)
    out = g_strdup("");
  g_object_unref(p);
  g_strchomp(out);
  return out;
}

static void write_key(const char *key, const char *value)
{
  GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL,
                                    "kwriteconfig6", "--file", "kwinrc",
                                    "--group", "Effect-poxicle_kwin",
                                    "--key", key, value, NULL);
  if (p) {
    g_subprocess_wait(p, NULL, NULL);
    g_object_unref(p);
  }
}

/* C-locale float fragment (the effect parses with QString::toFloat == '.'),
 * %g-trimmed so the file reads 1.7 not 1.7000000476837158. */
static const char *fstr(char *buf, gsize n, double v)
{
  g_ascii_formatd(buf, (int) n, "%g", v);
  return buf;
}

static float as_float(const char *s, float dflt)
{
  if (!s || !*s) return dflt;
  char *end = NULL;
  double v = g_ascii_strtod(s, &end);
  return (end && end != s) ? (float) v : dflt;
}

static int as_int(const char *s, int dflt)
{
  if (!s || !*s) return dflt;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  return (end && end != s) ? (int) v : dflt;
}

/* ---- presets ---- */

void pox_tune_seed(const char *name, PoxTune *out)
{
  for (gsize i = 0; i < G_N_ELEMENTS(kSeeds); i++) {
    if (g_strcmp0(name, kSeeds[i].name) == 0) { *out = kSeeds[i].t; return; }
  }
  *out = kSeeds[1].t;   /* fall back to ambient */
}

void pox_io_load_preset(const char *name, PoxTune *out)
{
  pox_tune_seed(name, out);

  char *key = g_strconcat("Preset-", name, NULL);
  char *packed = read_key(key);
  g_free(key);

  if (*packed) {
    char **f = g_strsplit(packed, ";", -1);
    if (g_strv_length(f) >= 15) {
      out->speed            = as_float(f[0],  out->speed);
      out->thickness        = as_int  (f[1],  out->thickness);
      out->tail_length      = as_float(f[2],  out->tail_length);
      out->pulse_depth      = as_float(f[3],  out->pulse_depth);
      out->pulse_speed      = as_float(f[4],  out->pulse_speed);
      out->env_attack       = as_float(f[5],  out->env_attack);
      out->env_release      = as_float(f[6],  out->env_release);
      out->env_curve        = as_int  (f[7],  out->env_curve);
      out->release_mode     = as_int  (f[8],  out->release_mode);
      out->shape            = as_int  (f[9],  out->shape);
      out->gap              = as_int  (f[10], out->gap);
      out->thk_attack       = as_float(f[11], out->thk_attack);
      out->thk_release      = as_float(f[12], out->thk_release);
      out->thk_curve        = as_int  (f[13], out->thk_curve);
      out->thk_release_mode = as_int  (f[14], out->thk_release_mode);
    }
    g_strfreev(f);
  }
  g_free(packed);
}

void pox_io_save_preset(const char *name, const PoxTune *t)
{
  char b0[G_ASCII_DTOSTR_BUF_SIZE], b2[G_ASCII_DTOSTR_BUF_SIZE], b3[G_ASCII_DTOSTR_BUF_SIZE];
  char b4[G_ASCII_DTOSTR_BUF_SIZE], b5[G_ASCII_DTOSTR_BUF_SIZE], b6[G_ASCII_DTOSTR_BUF_SIZE];
  char b11[G_ASCII_DTOSTR_BUF_SIZE], b12[G_ASCII_DTOSTR_BUF_SIZE];

  char *packed = g_strdup_printf(
    "%s;%d;%s;%s;%s;%s;%s;%d;%d;%d;%d;%s;%s;%d;%d",
    fstr(b0, sizeof b0, t->speed), t->thickness,
    fstr(b2, sizeof b2, t->tail_length), fstr(b3, sizeof b3, t->pulse_depth),
    fstr(b4, sizeof b4, t->pulse_speed), fstr(b5, sizeof b5, t->env_attack),
    fstr(b6, sizeof b6, t->env_release), t->env_curve, t->release_mode,
    t->shape, t->gap,
    fstr(b11, sizeof b11, t->thk_attack), fstr(b12, sizeof b12, t->thk_release),
    t->thk_curve, t->thk_release_mode);

  char *key = g_strconcat("Preset-", name, NULL);
  write_key(key, packed);
  g_free(key);
  g_free(packed);
}

/* ---- rules ---- */

void pox_rule_free(PoxRule *r)
{
  if (!r) return;
  g_free(r->app_id);
  g_free(r->preset);
  g_free(r->color);
  g_free(r);
}

GPtrArray *pox_io_load_rules(void)
{
  GPtrArray *rules = g_ptr_array_new_with_free_func((GDestroyNotify) pox_rule_free);
  char *raw = read_key("Rules");
  if (!*raw) { g_free(raw); return rules; }

  char **lines = g_strsplit(raw, ",", -1);
  for (char **l = lines; *l; l++) {
    char **f = g_strsplit(*l, "|", -1);
    guint nf = g_strv_length(f);
    if (nf >= 2 && f[0][0]) {
      PoxRule *r = g_new0(PoxRule, 1);
      r->app_id  = g_strdup(g_strstrip(f[0]));
      r->preset  = g_strdup(g_strstrip(f[1]));
      r->reverse = as_int(f[2], 0);   /* 0 fwd, 1 rev, 2 loop */
      const char *col = (nf > 3) ? g_strstrip(f[3]) : "";
      r->color   = (*col) ? g_strdup(col) : NULL;
      r->shape          = as_int(f[4],  -1);
      r->gap            = as_int(f[5],  -1);
      r->speed          = as_int(f[6],   0);
      r->thickness      = as_int(f[7],   0);
      r->tail           = as_int(f[8],   0);
      r->attack         = as_int(f[9],   0);
      r->release        = as_int(f[10],  0);
      r->release_mode   = as_int(f[11], -1);
      r->thk_attack     = as_int(f[12],  0);
      r->thk_release    = as_int(f[13],  0);
      r->thk_release_mode = as_int(f[14], -1);
      r->palette        = (nf > 15) ? as_int(f[15], 0) : 0;   /* absent => Muted */
      g_ptr_array_add(rules, r);
    }
    g_strfreev(f);
  }
  g_strfreev(lines);
  g_free(raw);
  return rules;
}

void pox_io_save_rules(GPtrArray *rules)
{
  GString *out = g_string_new(NULL);
  for (guint i = 0; i < rules->len; i++) {
    PoxRule *r = g_ptr_array_index(rules, i);
    if (!r->app_id || !*r->app_id)
      continue;
    if (out->len)
      g_string_append_c(out, ',');
    /* appId|preset|rev|color|shape|gap|speed|thk|tail|atk|rel|relMode|tAtk|tRel|tRelMode|palette */
    g_string_append_printf(out, "%s|%s|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d",
                           r->app_id, r->preset ? r->preset : "ambient",
                           r->reverse, r->color ? r->color : "",
                           r->shape, r->gap, r->speed, r->thickness, r->tail,
                           r->attack, r->release, r->release_mode,
                           r->thk_attack, r->thk_release, r->thk_release_mode,
                           r->palette);
  }
  write_key("Rules", out->str);
  g_string_free(out, TRUE);
}

/* ---- active-window target (kwinrc "Active": a rule line with no app id) ---- */

PoxRule *pox_io_load_active(void)
{
  PoxRule *r = g_new0(PoxRule, 1);
  r->preset = g_strdup("none");           /* unset => disabled */
  r->shape = r->gap = r->release_mode = r->thk_release_mode = -1;

  char *raw = read_key("Active");
  if (*raw) {
    char **f = g_strsplit(raw, "|", -1);
    guint nf = g_strv_length(f);
    /* preset|rev|color|shape|gap|speed|thk|tail|atk|rel|relMode|tAtk|tRel|tRelMode|palette */
    if (nf >= 1 && f[0][0]) {
      g_free(r->preset);
      r->preset = g_strdup(g_strstrip(f[0]));
    }
    if (nf >= 14) {
      r->reverse = as_int(f[1], 0);
      const char *col = g_strstrip(f[2]);
      r->color   = (*col) ? g_strdup(col) : NULL;
      r->shape          = as_int(f[3],  -1);
      r->gap            = as_int(f[4],  -1);
      r->speed          = as_int(f[5],   0);
      r->thickness      = as_int(f[6],   0);
      r->tail           = as_int(f[7],   0);
      r->attack         = as_int(f[8],   0);
      r->release        = as_int(f[9],   0);
      r->release_mode   = as_int(f[10], -1);
      r->thk_attack     = as_int(f[11],  0);
      r->thk_release    = as_int(f[12],  0);
      r->thk_release_mode = as_int(f[13], -1);
      r->palette        = (nf > 14) ? as_int(f[14], 0) : 0;   /* absent => Muted */
    }
    g_strfreev(f);
  }
  g_free(raw);
  return r;
}

void pox_io_save_active(const PoxRule *r)
{
  /* Same field order as a Rules entry but with NO leading app id. */
  char *packed = g_strdup_printf("%s|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d",
                                 r->preset ? r->preset : "none",
                                 r->reverse, r->color ? r->color : "",
                                 r->shape, r->gap, r->speed, r->thickness, r->tail,
                                 r->attack, r->release, r->release_mode,
                                 r->thk_attack, r->thk_release, r->thk_release_mode,
                                 r->palette);
  write_key("Active", packed);
  g_free(packed);
}

char *pox_io_load_default_preset(void)
{
  char *v = read_key("DefaultPreset");
  if (!*v) { g_free(v); return g_strdup("ambient"); }
  return v;
}

void pox_io_save_default_preset(const char *name)
{
  write_key("DefaultPreset", name);
}

/* ---- live reload ---- */

void pox_io_reconfigure(void)
{
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus)
    return;
  g_dbus_connection_call_sync(
    bus, "org.kde.KWin", "/Effects", "org.kde.kwin.Effects",
    "reconfigureEffect", g_variant_new("(s)", "poxicle_kwin"),
    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  g_object_unref(bus);
}

/* ---- interactive window picker ---- */

typedef struct { PoxPickCb cb; gpointer data; } PickCtx;

static void pick_window_reply(GObject *src, GAsyncResult *res, gpointer data)
{
  PickCtx *ctx = data;
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  char *app_id = NULL;

  if (ret) {
    /* queryWindowInfo -> (a{sv}); pull the app id out of the property map. */
    GVariant *info = NULL;
    g_variant_get(ret, "(@a{sv})", &info);
    /* resourceClass is the app id the effect matches on (windowClass contains
     * it); fall back to resourceName for clients that only set that. */
    if (!g_variant_lookup(info, "resourceClass", "s", &app_id))
      g_variant_lookup(info, "resourceName", "s", &app_id);
    g_variant_unref(info);
    g_variant_unref(ret);
  } else {
    /* Esc / no window under the cursor comes back as a DBus error: treat as a
     * silent cancel rather than surfacing it. */
    g_clear_error(&err);
  }

  ctx->cb(app_id, ctx->data);
  g_free(app_id);
  g_free(ctx);
}

void pox_io_pick_window(PoxPickCb cb, gpointer user_data)
{
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus) { cb(NULL, user_data); return; }

  PickCtx *ctx = g_new0(PickCtx, 1);
  ctx->cb = cb;
  ctx->data = user_data;
  /* queryWindowInfo blocks inside KWin until the user clicks, so call it async
   * (keeps the GTK UI responsive) with a long timeout so a slow, deliberate
   * click is never cut off by the default 25s DBus timeout. */
  g_dbus_connection_call(
    bus, "org.kde.KWin", "/KWin", "org.kde.KWin", "queryWindowInfo",
    NULL, G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE,
    G_MAXINT, NULL, pick_window_reply, ctx);
  g_object_unref(bus);
}
