/* poxicle.c — simulation core (toolkit/GL/Wayland-free).
 *
 * Ported from Chiguiro's edge engine (kgx-edge-draw.c / kgx-edge-burst.c /
 * kgx-particle.c), simplified to a single full-surface emitter: there is no
 * per-side widget split here, so every block is emitted directly. Output is a
 * flat PoxInstance[] for any renderer backend. Time is advanced via dt (the host
 * frame delta); all scheduling is in engine-local seconds.
 */
#include "poxicle.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define POX_MAX_BURSTS 8
#define POX_BASE_SEG   200.0   /* tail base length, px (matches BASE_OVERSCROLL_SEG) */

typedef struct {
  double      progress;    /* -1 inactive, else 0..1 eased */
  double      head;        /* perimeter position, px */
  PoxColor    color;
  double      due_s;       /* scheduled fire time (0 = none) */
  double      start_s;     /* 0 = inactive */
  double      duration_s;
  PoxTunables tune;        /* snapshot at fire time */
} PoxBurst;

struct PoxEngine {
  int         width, height, scale;
  double      perim;

  PoxTunables tune;            /* global / ambient stream */

  int         ambient;         /* auto-fire bursts */
  double      ambient_due_s;
  int         burst_count;     /* ambient fan: slots [0, burst_count) */
  double      burst_spread;

  PoxBurst    bursts[POX_MAX_BURSTS];

  int         os_edge;         /* overscroll edge, -1 = none */
  double      os_progress;     /* <0 = clear */
  PoxColor    os_color;
  double      os_start_s;      /* >0 = self-animating timeline active */
  double      os_duration_s;

  PoxColor    palette[POX_PALETTE_MAX];  /* ambient/fireworks burst colours */
  int         palette_n;       /* 0 => use the built-in muted default */

  int         kind;            /* PoxKind: emission pattern */
  int         reverse;         /* geometric kinds: current travel direction (0/1) */
  int         reverse_mode;    /* 0 fwd, 1 rev, 2 loop (alternate each cycle) */
  double      proc_progress;   /* geometric timeline, -1 = inactive */
  double      proc_start_s;
  double      proc_duration_s;
  int         proc_linear;     /* linear vs ease-out-cubic progress */
  PoxColor    proc_color;      /* solid tint for geometric / scroll kinds (palette "Solid") */
  PoxTunables proc_tune;       /* tunables snapshot taken at set_kind time */
  int         proc_cycle;      /* loop counter; advances the palette colour per cycle */

  double      now_s;
  uint32_t    rng;
};

/* ---- small PRNG (no glib) ---- */
static uint32_t rnd(PoxEngine *e)
{
  uint32_t x = e->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  e->rng = x ? x : 0x1234567u;
  return e->rng;
}
static double rnd01(PoxEngine *e) { return (rnd(e) >> 8) * (1.0 / 16777216.0); }
static int rnd_range(PoxEngine *e, int lo, int hi)
{ return hi <= lo ? lo : lo + (int) (rnd(e) % (uint32_t) (hi - lo)); }

/* ---- colour palettes ---------------------------------------------------- */
/* Built-in burst palettes for ambient/fireworks. Index 0 ("Muted") is the
 * historical default, so an engine on the default palette looks unchanged.
 * Ordered loosely by family (muted, warm, cool, green, vibrant, pastel, mono)
 * so the configurator dropdown reads in coherent groups. */
typedef struct { const char *name; PoxColor cols[POX_PALETTE_MAX]; int n; } PoxPalette;

static const PoxPalette POX_BUILTIN_PALETTES[] = {
  { "Muted",     { {0.85f,0.40f,0.75f,1}, {0.40f,0.60f,0.90f,1},
                   {0.40f,0.80f,0.50f,1}, {0.95f,0.65f,0.35f,1} }, 4 },
  { "Sunset",    { {1.00f,0.42f,0.42f,1}, {0.93f,0.35f,0.44f,1}, {0.95f,0.62f,0.30f,1},
                   {0.98f,0.78f,0.31f,1}, {0.77f,0.27f,0.21f,1} }, 5 },
  { "Ember",     { {0.42f,0.02f,0.06f,1}, {0.62f,0.01f,0.03f,1}, {0.82f,0.00f,0.00f,1},
                   {0.91f,0.36f,0.02f,1}, {0.98f,0.64f,0.03f,1} }, 5 },
  { "Autumn",    { {0.35f,0.18f,0.05f,1}, {0.50f,0.31f,0.14f,1}, {0.58f,0.40f,0.22f,1},
                   {0.65f,0.54f,0.39f,1}, {0.71f,0.68f,0.56f,1} }, 5 },
  { "Ocean",     { {0.00f,0.19f,0.29f,1}, {0.00f,0.47f,0.71f,1}, {0.00f,0.71f,0.85f,1},
                   {0.56f,0.88f,0.94f,1}, {0.79f,0.94f,0.97f,1} }, 5 },
  { "Glacier",   { {0.17f,0.49f,0.63f,1}, {0.27f,0.56f,0.69f,1}, {0.38f,0.65f,0.76f,1},
                   {0.54f,0.76f,0.85f,1}, {0.66f,0.84f,0.90f,1} }, 5 },
  { "Twilight",  { {0.14f,0.05f,0.30f,1}, {0.24f,0.04f,0.42f,1}, {0.35f,0.09f,0.60f,1},
                   {0.48f,0.17f,0.75f,1}, {0.62f,0.31f,0.87f,1} }, 5 },
  { "Forest",    { {0.03f,0.11f,0.08f,1}, {0.11f,0.26f,0.20f,1}, {0.18f,0.42f,0.31f,1},
                   {0.25f,0.57f,0.42f,1}, {0.45f,0.78f,0.62f,1} }, 5 },
  { "Moss",      { {0.20f,0.31f,0.25f,1}, {0.23f,0.35f,0.25f,1}, {0.35f,0.50f,0.34f,1},
                   {0.64f,0.69f,0.54f,1}, {0.85f,0.84f,0.80f,1} }, 5 },
  { "Neon",      { {1.00f,0.00f,0.43f,1}, {0.98f,0.34f,0.03f,1}, {1.00f,0.75f,0.04f,1},
                   {0.51f,0.22f,0.93f,1}, {0.23f,0.53f,1.00f,1} }, 5 },
  { "Pop",       { {0.98f,0.25f,0.27f,1}, {0.95f,0.45f,0.17f,1}, {0.97f,0.59f,0.12f,1},
                   {0.56f,0.75f,0.43f,1}, {0.26f,0.67f,0.55f,1} }, 5 },
  { "Candy",     { {1.00f,0.36f,0.56f,1}, {1.00f,0.59f,0.72f,1}, {1.00f,0.70f,0.78f,1},
                   {1.00f,0.78f,0.87f,1}, {0.80f,0.71f,0.86f,1} }, 5 },
  { "Rainbow",   { {0.91f,0.12f,0.18f,1}, {0.99f,0.55f,0.10f,1}, {0.98f,0.82f,0.16f,1},
                   {0.18f,0.71f,0.34f,1}, {0.16f,0.45f,0.80f,1}, {0.60f,0.20f,0.72f,1} }, 6 },
  { "Bloom",     { {0.80f,0.71f,0.86f,1}, {1.00f,0.78f,0.87f,1}, {1.00f,0.69f,0.80f,1},
                   {0.74f,0.88f,1.00f,1}, {0.64f,0.82f,1.00f,1} }, 5 },
  { "Sorbet",    { {0.98f,0.97f,0.80f,1}, {0.99f,0.89f,0.81f,1}, {1.00f,0.81f,0.82f,1},
                   {0.95f,0.75f,0.91f,1}, {0.81f,0.73f,0.94f,1} }, 5 },
  { "Mono Blue", { {0.01f,0.02f,0.37f,1}, {0.01f,0.24f,0.54f,1}, {0.00f,0.47f,0.71f,1},
                   {0.00f,0.59f,0.78f,1}, {0.28f,0.79f,0.89f,1} }, 5 },
  { "Mono Pink", { {0.35f,0.05f,0.13f,1}, {0.50f,0.06f,0.18f,1}, {0.64f,0.07f,0.24f,1},
                   {0.79f,0.09f,0.29f,1}, {1.00f,0.30f,0.43f,1} }, 5 },
  /* Soft/analogous additions — adjacent hues, low saturation spread, no opposing
   * colours. Appended (not slotted into the family groups above) so existing
   * stored palette ids stay stable. */
  { "Dusk",      { {0.30f,0.31f,0.46f,1}, {0.38f,0.36f,0.55f,1}, {0.47f,0.43f,0.63f,1},
                   {0.57f,0.52f,0.71f,1}, {0.69f,0.65f,0.80f,1} }, 5 },
  { "Slate",     { {0.28f,0.34f,0.42f,1}, {0.36f,0.43f,0.52f,1}, {0.46f,0.54f,0.63f,1},
                   {0.58f,0.65f,0.74f,1}, {0.72f,0.78f,0.85f,1} }, 5 },
  { "Seafoam",   { {0.27f,0.49f,0.46f,1}, {0.36f,0.59f,0.55f,1}, {0.48f,0.69f,0.64f,1},
                   {0.62f,0.79f,0.73f,1}, {0.77f,0.88f,0.82f,1} }, 5 },
  { "Lilac",     { {0.55f,0.44f,0.60f,1}, {0.64f,0.52f,0.68f,1}, {0.74f,0.62f,0.76f,1},
                   {0.83f,0.72f,0.82f,1}, {0.90f,0.81f,0.87f,1} }, 5 },
  { "Sand",      { {0.55f,0.45f,0.33f,1}, {0.67f,0.56f,0.42f,1}, {0.78f,0.68f,0.53f,1},
                   {0.87f,0.79f,0.65f,1}, {0.93f,0.88f,0.77f,1} }, 5 },
  { "Clay",      { {0.55f,0.34f,0.27f,1}, {0.66f,0.43f,0.34f,1}, {0.76f,0.53f,0.43f,1},
                   {0.84f,0.64f,0.54f,1}, {0.90f,0.76f,0.67f,1} }, 5 },
};
static const int POX_BUILTIN_PALETTE_N =
    (int) (sizeof POX_BUILTIN_PALETTES / sizeof POX_BUILTIN_PALETTES[0]);

int pox_palette_count(void) { return POX_BUILTIN_PALETTE_N; }

const char *pox_palette_name(int id)
{
  return (id >= 0 && id < POX_BUILTIN_PALETTE_N) ? POX_BUILTIN_PALETTES[id].name : NULL;
}

int pox_palette_colors(int id, PoxColor *out, int max)
{
  if (id < 0 || id >= POX_BUILTIN_PALETTE_N || !out || max <= 0) return 0;
  int n = POX_BUILTIN_PALETTES[id].n;
  if (n > max) n = max;
  for (int i = 0; i < n; i++) out[i] = POX_BUILTIN_PALETTES[id].cols[i];
  return n;
}

/* Pick the next burst colour: sample the engine's active palette, or fall back
 * to the built-in Muted set when none has been assigned (palette_n == 0). */
static PoxColor burst_palette_color(PoxEngine *e)
{
  if (e->palette_n > 0)
    return e->palette[rnd(e) % (uint32_t) e->palette_n];
  return POX_BUILTIN_PALETTES[0].cols[rnd(e) % (uint32_t) POX_BUILTIN_PALETTES[0].n];
}

/* Pick a palette colour for a geometric/scroll preset segment. `key` selects the
 * colour deterministically (per-segment index for multi-arm kinds, per-cycle for
 * single-arm) so the look is stable frame-to-frame, not strobing. The "Solid"
 * opt-out resolves to a 1-colour palette upstream, so it lands here and returns
 * that single colour; `fallback` covers the no-palette-assigned case. */
static PoxColor preset_palette_color(const PoxEngine *e, int key, PoxColor fallback)
{
  if (e->palette_n > 0)
    return e->palette[((unsigned) key) % (unsigned) e->palette_n];
  return fallback;
}

/* ---- envelopes (ported) ---- */
static float pox_envelope(double t, double attack, double release, int curve)
{
  float lin;
  if (attack > 0.0 && t < attack) {
    lin = (float) (t / attack);
    if (curve == 1) return sqrtf(lin);
    if (curve == 3) return lin * lin;
    return lin;
  }
  if (release > 0.0 && t > 1.0 - release) {
    lin = (float) ((1.0 - t) / release);
    if (curve == 1) return sqrtf(lin);
    if (curve == 3) return lin * lin;
    return lin;
  }
  return 1.0f;
}

void pox_tunables_default(PoxTunables *t)
{
  t->speed            = 1.0f;
  t->thickness        = 10;
  t->tail_length      = 1.0f;
  t->pulse_depth      = 0.3f;
  t->pulse_speed      = 1.5f;
  t->env_attack       = 0.12f;
  t->env_release      = 0.20f;
  t->env_curve        = 2;
  t->release_mode     = POX_RELEASE_UNIFORM;
  t->shape            = POX_SHAPE_TRIANGLE;   /* matches Chiguiro's default edge-shape */
  t->gap              = 0;                     /* gapped → distinct particles */
  t->thk_attack       = 0.0f;
  t->thk_release      = 0.0f;
  t->thk_curve        = 2;
  t->thk_release_mode = POX_RELEASE_UNIFORM;
}

/* ---- one trailing segment → instances (ported from kgx_edge_draw_segment,
 * minus the four-widget side cull: this surface covers the whole perimeter) ---- */
static void emit_segment(PoxEngine *e, PoxInstance *out, size_t *count, size_t cap,
                         double head_d, double seg_len, float alpha, PoxColor color,
                         int trail_dir, const PoxTunables *tune, double phase)
{
  double w = e->width, h = e->height, perim = e->perim;
  double base_blk = (double) tune->thickness;
  double blk = base_blk;
  float thk_attack_env = 1.0f, thk_release_factor = 0.0f;

  if (tune->thk_attack > 0.0)
    thk_attack_env = pox_envelope(phase, tune->thk_attack, 0.0, tune->thk_curve);

  if (tune->thk_release_mode == POX_RELEASE_RETRACT) {
    blk *= pox_envelope(phase, tune->thk_attack, tune->thk_release, tune->thk_curve);
  } else {
    blk *= thk_attack_env;
    if ((tune->thk_release_mode == POX_RELEASE_SPREAD ||
         tune->thk_release_mode == POX_RELEASE_GROW ||
         tune->thk_release_mode == POX_RELEASE_ALL) &&
        tune->thk_release > 0.0 && phase > 1.0 - tune->thk_release)
      thk_release_factor = (float) ((phase - (1.0 - tune->thk_release)) / tune->thk_release);
  }
  if (blk < 1.0) blk = 1.0;

  double gap = tune->gap ? 0.0 : base_blk;
  if (tune->release_mode == POX_RELEASE_SPREAD &&
      tune->env_release > 0.0 && phase > 1.0 - tune->env_release) {
    double rt = (phase - (1.0 - tune->env_release)) / tune->env_release;
    gap *= (1.0 + rt * 3.0);
  }

  double step = blk + gap;
  int blocks = (int) (seg_len / step);
  if (blocks < 1) blocks = 1;

  double d = fmod(head_d + perim, perim);
  double delta = trail_dir * step;
  float inv_blocks = 1.0f / (float) (blocks > 1 ? blocks - 1 : 1);
  float phase_offset = (float) (phase * 40.0 * tune->pulse_speed);

  for (int s = 0; s < blocks; s++) {
    if (*count >= cap) break;
    if (s > 0) { d += delta; if (d >= perim) d -= perim; if (d < 0.0) d += perim; }

    float t = (float) s * inv_blocks;
    double block_blk = blk;
    if (thk_release_factor > 0.0f) {
      if (tune->thk_release_mode == POX_RELEASE_ALL)
        block_blk = blk * (1.0 - (double) thk_release_factor);
      else if (tune->thk_release_mode == POX_RELEASE_GROW)
        block_blk = blk * (1.0 + (double) (t * thk_release_factor));
      else
        block_blk = blk * (1.0 - (double) (t * thk_release_factor));
      if (block_blk < 1.0) block_blk = 1.0;
    }

    float bb = (float) block_blk;
    float px, py, tri = 0.0f;
    if (d < w)            { px = (float) d;                       py = 0.0f;            tri = (trail_dir == -1) ? 0.0f   : 180.0f; }
    else if (d < w + h)   { px = (float) (w - bb);                py = (float)(d - w);  tri = (trail_dir == -1) ? 90.0f  : 270.0f; }
    else if (d < 2*w + h) { px = (float)(w - (d - w - h) - bb);   py = (float)(h - bb); tri = (trail_dir == -1) ? 180.0f : 0.0f;   }
    else                  { px = 0.0f;                            py = (float)(h - (d - 2*w - h) - bb); tri = (trail_dir == -1) ? 270.0f : 90.0f; }

    /* Hug the inside edge through corners. Each edge's parameterisation runs a
     * block past its far corner (top: px=d -> w+bb; bottom/left: px/py -> -bb),
     * so a block straddling a turn pokes outside the window rect. Chiguiro gets
     * this for free — it draws into four per-side widgets and clips each block
     * to its strip. This single full-surface overlay has no such clip, so clamp
     * the block fully inside [0,w-bb] x [0,h-bb] here instead; otherwise corner
     * blocks bleed out on turning. */
    {
      float max_x = (float) w - bb, max_y = (float) h - bb;
      if (max_x < 0.0f) max_x = 0.0f;
      if (max_y < 0.0f) max_y = 0.0f;
      if (px < 0.0f) px = 0.0f; else if (px > max_x) px = max_x;
      if (py < 0.0f) py = 0.0f; else if (py > max_y) py = max_y;
    }

    float a = alpha * (1.0f - 0.7f * t);
    if (s > 0 && tune->pulse_depth > 0.0) {
      float intensity = t * (float) tune->pulse_depth;
      float pulse = 1.0f - intensity + intensity * sinf((float) s * 1.2f - phase_offset);
      a *= pulse;
    }

    PoxInstance *in = &out[(*count)++];
    in->x = px; in->y = py; in->size = bb;
    in->angle = (tune->shape == POX_SHAPE_TRIANGLE) ? tri : 0.0f;
    in->shape = tune->shape;
    in->color = color;
    in->color.a = a;
  }
}

/* A burst emits two segments spreading out from its head. */
static void emit_burst(PoxEngine *e, const PoxBurst *b,
                       PoxInstance *out, size_t *count, size_t cap)
{
  const PoxTunables *tt = &b->tune;
  double p = b->progress;
  float  b_env  = pox_envelope(p, tt->env_attack, tt->env_release, tt->env_curve);
  double seg_env = pox_envelope(p, tt->env_attack, 0.0, tt->env_curve);
  double seg    = POX_BASE_SEG * tt->tail_length * 2.0 * seg_env;
  float  a      = b_env * 0.5f;
  double spread = seg * 3.0 * p;
  double lh = fmod(b->head - spread + e->perim, e->perim);
  double rh = fmod(b->head + spread, e->perim);

  emit_segment(e, out, count, cap, lh, seg, a, b->color, +1, tt, p);
  emit_segment(e, out, count, cap, rh, seg, a, b->color, -1, tt, p);
}

/* ---- burst lifecycle ---- */
static int burst_active(const PoxBurst *b)
{ return b->start_s > 0.0 || b->due_s > 0.0 || b->progress >= 0.0; }

static void burst_begin(PoxEngine *e, int i, double head, PoxColor c)
{
  PoxBurst *b = &e->bursts[i];
  double spd = e->tune.speed > 0.0f ? e->tune.speed : 1.0;
  b->due_s = 0.0;
  b->head = head;
  b->color = c;
  b->tune = e->tune;
  b->duration_s = 0.8 / spd;
  b->start_s = e->now_s;
  b->progress = 0.0;
}

static void ambient_schedule(PoxEngine *e)
{
  if (e->ambient_due_s > 0.0 || !e->ambient) return;
  double spd = e->tune.speed > 0.0f ? e->tune.speed : 1.0;
  int lo = (int) (600 * e->burst_spread / spd);
  int hi = (int) (1200 * e->burst_spread / spd);
  int any = 0;
  for (int i = 0; i < e->burst_count; i++)
    if (burst_active(&e->bursts[i])) { any = 1; break; }
  int delay = !any ? 200 : rnd_range(e, lo > 200 ? lo : 200, hi > 400 ? hi : 400);
  e->ambient_due_s = e->now_s + delay / 1000.0;
}

static void ambient_fire(PoxEngine *e)
{
  e->ambient_due_s = 0.0;
  if (!e->ambient) return;

  burst_begin(e, 0, rnd01(e) * e->perim, burst_palette_color(e));

  for (int i = 1; i < e->burst_count; i++) {
    if (burst_active(&e->bursts[i])) continue;
    int lo = (int) (150 * i * e->burst_spread);
    int hi = (int) (600 * i * e->burst_spread);
    e->bursts[i].due_s = e->now_s + rnd_range(e, lo > 50 ? lo : 50, hi > 100 ? hi : 100) / 1000.0;
  }
  ambient_schedule(e);
}

static void advance(PoxEngine *e, PoxBurst *b)
{
  if (b->start_s <= 0.0 || b->progress < 0.0) return;
  double raw = b->duration_s > 0.0
             ? (e->now_s - b->start_s) / b->duration_s : 1.0;
  if (raw < 0.0) raw = 0.0;
  if (raw >= 1.0) { b->progress = -1.0; b->start_s = 0.0; b->duration_s = 0.0; return; }
  double inv = 1.0 - raw;
  b->progress = 1.0 - inv * inv * inv;   /* ease-out cubic */
}

/* ---- public API ---- */
PoxEngine *pox_engine_new(void)
{
  PoxEngine *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  e->scale = 1;
  e->rng = 0x1234567u;
  e->burst_count = 4;
  e->burst_spread = 1.0;
  e->os_edge = -1;
  e->os_progress = -1.0;
  e->kind = POX_KIND_NONE;
  e->proc_progress = -1.0;
  for (int i = 0; i < POX_MAX_BURSTS; i++)
    e->bursts[i].progress = -1.0;
  pox_tunables_default(&e->tune);
  return e;
}

void pox_engine_free(PoxEngine *e) { free(e); }

void pox_engine_set_surface(PoxEngine *e, int width, int height, int scale)
{
  e->width = width; e->height = height; e->scale = scale > 0 ? scale : 1;
  e->perim = 2.0 * (width + height);
}

void pox_engine_set_tunables(PoxEngine *e, const PoxTunables *t) { e->tune = *t; }

void pox_engine_set_ambient(PoxEngine *e, int enabled)
{
  if (e->ambient == !!enabled) return;
  e->ambient = !!enabled;
  if (e->ambient && e->ambient_due_s == 0.0)
    e->ambient_due_s = e->now_s + 0.05;
}

void pox_engine_set_palette_colors(PoxEngine *e, const PoxColor *cols, int n)
{
  if (!e) return;
  if (!cols || n <= 0) { e->palette_n = 0; return; }   /* reset to built-in default */
  if (n > POX_PALETTE_MAX) n = POX_PALETTE_MAX;
  for (int i = 0; i < n; i++) e->palette[i] = cols[i];
  e->palette_n = n;
}

void pox_engine_set_palette(PoxEngine *e, int id)
{
  PoxColor tmp[POX_PALETTE_MAX];
  int n = pox_palette_colors(id, tmp, POX_PALETTE_MAX);
  if (n > 0) pox_engine_set_palette_colors(e, tmp, n);
}

/* Per-kind loop duration, ms (matches kgx_edge_set_process_particle). */
static double proc_duration_ms(int kind, double spd)
{
  if (spd <= 0.0) spd = 1.0;
  switch (kind) {
  case POX_KIND_ROTATE:    return 3500.0 / spd;
  case POX_KIND_PING_PONG: return 1200.0 / spd;
  case POX_KIND_CORNERS:
  case POX_KIND_PULSE_OUT: return 2500.0 / spd;
  default:                 return 3000.0 / spd;
  }
}

void pox_engine_set_kind(PoxEngine *e, PoxKind kind, int reverse, PoxColor color)
{
  if (!e) return;

  e->kind         = kind;
  /* reverse is tri-state: 0 forward, 1 reverse, 2 loop (alternate direction on
   * every geometric cycle, flipped at the re-arm in tick()). Loop starts
   * forward. */
  e->reverse_mode = reverse;
  e->reverse      = (reverse == 1) ? 1 : 0;
  /* a < 0 sentinel => no per-app color set; use a cool default tone. */
  e->proc_color = (color.a >= 0.0f) ? color
                                    : (PoxColor){ 0.55f, 0.78f, 1.0f, 1.0f };
  e->proc_cycle = 0;   /* restart palette stepping for this (re)attach */

  const int geometric = (kind == POX_KIND_CORNERS  || kind == POX_KIND_PULSE_OUT ||
                         kind == POX_KIND_ROTATE   || kind == POX_KIND_PING_PONG);

  if (geometric) {
    /* Snapshot the tunables (already set by the caller) and arm the looping
     * timeline; tick() re-arms it on completion so the motion is continuous. */
    pox_engine_set_ambient(e, 0);
    e->proc_tune       = e->tune;
    e->proc_duration_s = proc_duration_ms(kind, e->tune.speed) / 1000.0;
    e->proc_linear     = (kind == POX_KIND_ROTATE || kind == POX_KIND_PING_PONG);
    e->proc_start_s    = e->now_s;
    e->proc_progress   = 0.0;
  } else {
    e->proc_progress   = -1.0;
    e->proc_start_s    = 0.0;
    e->proc_duration_s = 0.0;
    /* AMBIENT/FIREWORKS drive the auto-burst fan; SCROLL loops an overscroll
     * beam from tick(); NONE is quiet. Give fireworks a denser, tighter fan. */
    e->burst_count  = (kind == POX_KIND_FIREWORKS) ? 6 : 4;
    e->burst_spread = (kind == POX_KIND_FIREWORKS) ? 0.6 : 1.0;
    pox_engine_set_ambient(e, (kind == POX_KIND_AMBIENT || kind == POX_KIND_FIREWORKS) ? 1 : 0);
  }
}

void pox_engine_burst(PoxEngine *e, float pos, PoxColor color)
{
  if (e->perim <= 0.0) return;
  for (int i = e->burst_count; i < POX_MAX_BURSTS; i++) {   /* manual slots */
    if (!burst_active(&e->bursts[i])) {
      double head = fmod((double) pos, 1.0);
      if (head < 0.0) head += 1.0;
      burst_begin(e, i, head * e->perim, color);
      return;
    }
  }
}

void pox_engine_overscroll(PoxEngine *e, PoxEdge edge, float progress, PoxColor color)
{
  e->os_edge = (progress < 0.0f) ? -1 : (int) edge;
  e->os_progress = (progress < 0.0f) ? -1.0 : progress;
  e->os_color = color;
  e->os_start_s = 0.0;   /* host-driven, no internal timeline */
}

void pox_engine_fire_overscroll(PoxEngine *e, PoxEdge edge, PoxColor color)
{
  if (!e) return;
  e->os_edge = (int) edge;
  e->os_color = color;
  e->os_progress = 0.0;
  e->os_start_s = e->now_s;
  e->os_duration_s = 0.6;   /* matches BASE_OVERSCROLL_MS */
}

static void emit_overscroll(PoxEngine *e, PoxInstance *out, size_t *count, size_t cap)
{
  const PoxTunables *tt = &e->tune;
  double w = e->width, h = e->height, perim = e->perim, prog = e->os_progress;
  float env = pox_envelope(prog, tt->env_attack, tt->env_release, tt->env_curve);
  float a = env * 0.9f;
  double corner, h_head, v_head;
  int h_trail, v_trail;

  /* Corner burst along two adjacent edges (style 0 of the original). */
  if (e->os_edge == POX_EDGE_BOTTOM) {
    corner = w + h;
    h_head = fmod(corner + w * prog, perim);
    v_head = fmod(corner - h * prog + perim, perim);
    h_trail = -1; v_trail = +1;
  } else {
    corner = w;
    h_head = fmod(corner - w * prog + perim, perim);
    v_head = fmod(corner + h * prog, perim);
    h_trail = +1; v_trail = -1;
  }
  emit_segment(e, out, count, cap, h_head, POX_BASE_SEG, a, e->os_color, h_trail, tt, prog);
  emit_segment(e, out, count, cap, v_head, POX_BASE_SEG, a, e->os_color, v_trail, tt, prog);
}

/* ---- geometric kinds (ported verbatim from kgx-edge.c's per-preset switch,
 * with kgx_edge_draw_segment -> emit_segment and the four-widget side cull
 * dropped). Driven by the looping proc_* timeline. ---- */
static void emit_kind(PoxEngine *e, PoxInstance *out, size_t *count, size_t cap)
{
  if (e->proc_progress < 0.0) return;

  const PoxTunables *pt = &e->proc_tune;
  double w = e->width, h = e->height, perim = e->perim;
  double p = e->proc_progress;
  float  env = pox_envelope(p, pt->env_attack, pt->env_release, pt->env_curve);
  float  a = env * 0.8f;
  double seg_full = POX_BASE_SEG * pt->tail_length * 2.0;
  double tail_env, seg;
  /* Geometric presets honour the active palette: multi-arm kinds (corners,
   * pulse-out) colour each arm by its segment index; single-arm kinds (rotate,
   * ping-pong) step one palette colour per loop cycle. "Solid" is a 1-colour
   * palette, so this transparently collapses back to the per-app proc_color. */
  PoxColor col_cycle = preset_palette_color(e, e->proc_cycle, e->proc_color);

  /* Tail envelope: rotate keeps a full tail (always in motion); retract pulls
   * the tail back faster than alpha fades; everything else grows on attack. */
  if (e->kind == POX_KIND_ROTATE) {
    tail_env = 1.0;
  } else if (pt->release_mode == POX_RELEASE_RETRACT) {
    float raw = pox_envelope(p, pt->env_attack, pt->env_release, 1);
    tail_env = (double) (raw * raw);
  } else {
    tail_env = pox_envelope(p, pt->env_attack, 0.0, pt->env_curve);
  }
  seg = seg_full * tail_env;

  switch (e->kind) {
  case POX_KIND_CORNERS: {
    double corner_a, corner_b, travel, clamped_seg, a_cw, a_ccw, b_cw, b_ccw;
    if (e->reverse) { corner_a = w;   corner_b = 2.0 * w + h; }
    else            { corner_a = 0.0; corner_b = w + h;       }
    travel = (w + h) / 2.0 * p;
    clamped_seg = seg < travel ? seg : travel;
    a_cw  = fmod(corner_a + travel, perim);
    a_ccw = fmod(corner_a - travel + perim * 2.0, perim);
    b_cw  = fmod(corner_b + travel, perim);
    b_ccw = fmod(corner_b - travel + perim * 2.0, perim);
    emit_segment(e, out, count, cap, a_cw,  clamped_seg, a, preset_palette_color(e, 0, e->proc_color), -1, pt, p);
    emit_segment(e, out, count, cap, a_ccw, clamped_seg, a, preset_palette_color(e, 1, e->proc_color), +1, pt, p);
    emit_segment(e, out, count, cap, b_cw,  clamped_seg, a, preset_palette_color(e, 2, e->proc_color), -1, pt, p);
    emit_segment(e, out, count, cap, b_ccw, clamped_seg, a, preset_palette_color(e, 3, e->proc_color), +1, pt, p);
    break;
  }
  case POX_KIND_PULSE_OUT: {
    double center, spread, clamped_seg, left_head, right_head;
    center = e->reverse ? (w + h + w / 2.0) : (w / 2.0);
    spread = (perim / 4.0) * p;
    clamped_seg = seg < spread ? seg : spread;
    left_head  = fmod(center - spread + perim, perim);
    right_head = fmod(center + spread, perim);
    emit_segment(e, out, count, cap, left_head,  clamped_seg, a, preset_palette_color(e, 0, e->proc_color), +1, pt, p);
    emit_segment(e, out, count, cap, right_head, clamped_seg, a, preset_palette_color(e, 1, e->proc_color), -1, pt, p);
    break;
  }
  case POX_KIND_ROTATE: {
    int    dir   = e->reverse ? -1 : +1;
    int    trail = e->reverse ? +1 : -1;
    double half_p = fmod(p * 2.0, 1.0);
    double eased  = 1.0 - (1.0 - half_p) * (1.0 - half_p) * (1.0 - half_p);
    float  lap_env = pox_envelope(half_p, pt->env_attack, pt->env_release, pt->env_curve);
    float  lap_a   = lap_env * 0.8f;
    double lap_tail, lap_seg, travel, clamped_seg, offset, head;
    if (pt->release_mode == POX_RELEASE_RETRACT) {
      float raw = pox_envelope(half_p, pt->env_attack, pt->env_release, 1);
      lap_tail = (double) (raw * raw);
    } else {
      lap_tail = pox_envelope(half_p, pt->env_attack, 0.0, pt->env_curve);
    }
    lap_seg = seg_full * lap_tail;
    travel  = (perim / 2.0) * eased;
    clamped_seg = (lap_seg * 2.0) < travel ? (lap_seg * 2.0) : travel;
    offset = (p >= 0.5) ? perim / 2.0 : 0.0;
    head   = fmod(dir * travel + offset + perim, perim);
    emit_segment(e, out, count, cap, head, clamped_seg, lap_a, col_cycle, trail, pt, half_p);
    break;
  }
  case POX_KIND_PING_PONG: {
    double edge_start, half_p = fmod(p * 2.0, 1.0);
    int    returning = (p >= 0.5);
    float  pp_env = pox_envelope(half_p, pt->env_attack, pt->env_release, pt->env_curve);
    float  pp_a   = pp_env * 0.9f;
    double pp_tail, pp_seg, travel, clamped_seg, pos;
    int    trail;
    edge_start = e->reverse ? (w + h) : 0.0;
    if (pt->release_mode == POX_RELEASE_RETRACT) {
      float raw = pox_envelope(half_p, pt->env_attack, pt->env_release, 1);
      pp_tail = (double) (raw * raw);
    } else {
      pp_tail = pox_envelope(half_p, pt->env_attack, 0.0, pt->env_curve);
    }
    pp_seg = POX_BASE_SEG * pp_tail;
    travel = w * half_p;
    clamped_seg = pp_seg < travel ? pp_seg : travel;
    if (!returning) {
      pos   = fmod(edge_start + travel + perim, perim);
      trail = -1;
    } else {
      pos   = fmod(edge_start + w * (1.0 - half_p) + perim, perim);
      trail = +1;
    }
    emit_segment(e, out, count, cap, pos, clamped_seg, pp_a, col_cycle, trail, pt, half_p);
    break;
  }
  default:
    break;
  }
}

size_t pox_engine_tick(PoxEngine *e, double dt, PoxInstance *out, size_t cap)
{
  size_t count = 0;
  if (!e || e->perim <= 0.0 || !out || cap == 0) return 0;

  e->now_s += dt;

  /* fire scheduled ambient work */
  if (e->ambient) {
    if (e->ambient_due_s > 0.0 && e->now_s >= e->ambient_due_s)
      ambient_fire(e);
    for (int i = 0; i < e->burst_count; i++)
      if (e->bursts[i].due_s > 0.0 && e->now_s >= e->bursts[i].due_s)
        burst_begin(e, i, rnd01(e) * e->perim, burst_palette_color(e));
    if (e->ambient_due_s == 0.0)
      ambient_schedule(e);
  }

  /* advance + emit all active bursts */
  for (int i = 0; i < POX_MAX_BURSTS; i++) {
    advance(e, &e->bursts[i]);
    if (e->bursts[i].progress >= 0.0)
      emit_burst(e, &e->bursts[i], out, &count, cap);
  }

  /* geometric-kind timeline: advance, then loop for a continuous per-window look.
   * proc_progress >= 0 is the "armed" flag; proc_start_s may legitimately be 0
   * (the engine clock starts at 0), so it must not gate this. */
  if (e->proc_progress >= 0.0 && e->proc_duration_s > 0.0) {
    double raw = (e->now_s - e->proc_start_s) / e->proc_duration_s;
    if (raw < 0.0) raw = 0.0;
    if (raw >= 1.0) {                          /* re-arm: loop the animation */
      e->proc_start_s  = e->now_s;
      e->proc_progress = 0.0;
      e->proc_cycle++;                         /* step the palette colour each cycle */
      if (e->reverse_mode == 2)                /* loop: alternate direction */
        e->reverse = !e->reverse;
    } else {
      e->proc_progress = e->proc_linear
                       ? raw
                       : 1.0 - (1.0 - raw) * (1.0 - raw) * (1.0 - raw);
    }
  }
  emit_kind(e, out, &count, cap);

  /* scroll kind: keep re-firing an overscroll beam so it reads as a loop, each
   * fire stepping the palette (host-driven overscroll keeps its own colour). */
  if (e->kind == POX_KIND_SCROLL && e->os_edge < 0)
    pox_engine_fire_overscroll(e, POX_EDGE_TOP,
                               preset_palette_color(e, e->proc_cycle++, e->proc_color));

  /* self-animating overscroll timeline */
  if (e->os_start_s > 0.0 && e->os_edge >= 0) {
    double raw = e->os_duration_s > 0.0
               ? (e->now_s - e->os_start_s) / e->os_duration_s : 1.0;
    if (raw < 0.0) raw = 0.0;
    if (raw >= 1.0) {
      e->os_edge = -1; e->os_progress = -1.0; e->os_start_s = 0.0;
    } else {
      double inv = 1.0 - raw;
      e->os_progress = 1.0 - inv * inv * inv;
    }
  }

  if (e->os_edge >= 0 && e->os_progress >= 0.0)
    emit_overscroll(e, out, &count, cap);

  return count;
}

int pox_engine_active(const PoxEngine *e)
{
  if (!e) return 0;
  if (e->ambient || e->ambient_due_s > 0.0) return 1;
  if (e->proc_progress >= 0.0) return 1;            /* geometric kind looping */
  if (e->kind == POX_KIND_SCROLL) return 1;         /* scroll keeps re-firing */
  if (e->os_edge >= 0 && e->os_progress >= 0.0) return 1;
  for (int i = 0; i < POX_MAX_BURSTS; i++)
    if (burst_active(&e->bursts[i])) return 1;
  return 0;
}
