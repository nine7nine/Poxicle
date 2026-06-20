/* pox-presets-page.c — per-preset tunables editor, in Chiguiro's compact style:
 * slim numeric cells + flat single-glyph cycle buttons. Apply writes each
 * Preset-<name> to kwinrc and reloads the effect live. */
#include "pox-config.h"
#include "pox-cells.h"

typedef enum {
  F_SPEED, F_THK, F_TAIL, F_PDEP, F_PSPD, F_ATK, F_REL, F_CURVE,
  F_RMODE, F_SHAPE, F_GAP, F_TATK, F_TREL, F_TCURVE, F_TRMODE, N_FIELDS
} Field;

/* glyphs (shared with the Apps page conventions) */
static const char *const g_shape[] = { "■", "●", "◆", "▶" }; /* ■ ● ◆ ▶ */
static const char *const g_gap[]   = { "□", "■" };                     /* 0=□ gapped, 1=■ solid (engine order) */
static const char *const g_curve[] = { "(", "/", ")" };                          /* base 1: concave/linear/convex */
static const char *const g_rmode[] = { "U", "R", "S", "G", "A" };                /* uniform/retract/spread/grow/all */

typedef struct {
  const char         *header, *tooltip;
  gboolean            is_enum;
  const char *const  *glyphs;
  int                 n_glyphs, base;
  const char         *css;
  double              lo, hi, step;
  int                 digits;
} ColDesc;

static const ColDesc cols[N_FIELDS] = {
  [F_SPEED]  = { "Spd",   "Speed multiplier",       FALSE, NULL,0,0,NULL, 0.1, 8.0,  0.1, 1 },
  [F_THK]    = { "Thk",   "Block thickness (px)",   FALSE, NULL,0,0,NULL, 2,   100,  1,   0 },
  [F_TAIL]   = { "Tail",  "Tail length multiplier", FALSE, NULL,0,0,NULL, 0.1, 10.0, 0.1, 1 },
  [F_PDEP]   = { "P.Dep", "Shimmer depth",          FALSE, NULL,0,0,NULL, 0.0, 1.0,  0.05,2 },
  [F_PSPD]   = { "P.Spd", "Shimmer speed",          FALSE, NULL,0,0,NULL, 0.1, 10.0, 0.1, 1 },
  [F_ATK]    = { "Atk",   "Envelope attack",        FALSE, NULL,0,0,NULL, 0.0, 1.0,  0.05,2 },
  [F_REL]    = { "Rel",   "Envelope release",       FALSE, NULL,0,0,NULL, 0.0, 1.0,  0.05,2 },
  [F_CURVE]  = { "Crv",   "Envelope curve",         TRUE, g_curve, 3, 1, "tune-curve",   0,0,0,0 },
  [F_RMODE]  = { "Rls",   "Release mode (U/R/S)",   TRUE, g_rmode, 3, 0, "tune-rls",     0,0,0,0 },
  [F_SHAPE]  = { "Shape", "Particle shape",         TRUE, g_shape, 4, 0, "tune-shape",   0,0,0,0 },
  [F_GAP]    = { "Gap",   "Solid / gapped",         TRUE, g_gap,   2, 0, "tune-gap",     0,0,0,0 },
  [F_TATK]   = { "T.Atk", "Thickness attack",       FALSE, NULL,0,0,NULL, 0.0, 1.0,  0.05,2 },
  [F_TREL]   = { "T.Rel", "Thickness release",      FALSE, NULL,0,0,NULL, 0.0, 1.0,  0.05,2 },
  [F_TCURVE] = { "T.Crv", "Thickness curve",        TRUE, g_curve, 3, 1, "tune-curve",   0,0,0,0 },
  [F_TRMODE] = { "T.Rls", "Thickness release mode", TRUE, g_rmode, 5, 0, "tune-thk-rls", 0,0,0,0 },
};

typedef struct {
  PoxSavedCb cb;
  gpointer   cb_data;
  GtkWidget *w[POX_N_PRESETS][N_FIELDS];
} PresetsPage;

static double tune_get(const PoxTune *t, Field f)
{
  switch (f) {
  case F_SPEED:  return t->speed;          case F_THK:    return t->thickness;
  case F_TAIL:   return t->tail_length;    case F_PDEP:   return t->pulse_depth;
  case F_PSPD:   return t->pulse_speed;    case F_ATK:    return t->env_attack;
  case F_REL:    return t->env_release;    case F_CURVE:  return t->env_curve;
  case F_RMODE:  return t->release_mode;   case F_SHAPE:  return t->shape;
  case F_GAP:    return t->gap;            case F_TATK:   return t->thk_attack;
  case F_TREL:   return t->thk_release;    case F_TCURVE: return t->thk_curve;
  case F_TRMODE: return t->thk_release_mode; default:     return 0;
  }
}

static void tune_set(PoxTune *t, Field f, double v)
{
  switch (f) {
  case F_SPEED:  t->speed = (float) v; break;        case F_THK:    t->thickness = (int) v; break;
  case F_TAIL:   t->tail_length = (float) v; break;  case F_PDEP:   t->pulse_depth = (float) v; break;
  case F_PSPD:   t->pulse_speed = (float) v; break;  case F_ATK:    t->env_attack = (float) v; break;
  case F_REL:    t->env_release = (float) v; break;  case F_CURVE:  t->env_curve = (int) v; break;
  case F_RMODE:  t->release_mode = (int) v; break;   case F_SHAPE:  t->shape = (int) v; break;
  case F_GAP:    t->gap = (int) v; break;            case F_TATK:   t->thk_attack = (float) v; break;
  case F_TREL:   t->thk_release = (float) v; break;  case F_TCURVE: t->thk_curve = (int) v; break;
  case F_TRMODE: t->thk_release_mode = (int) v; break; default: break;
  }
}

static double widget_get(GtkWidget *w, const ColDesc *c)
{
  return c->is_enum ? pox_cycle_value(w) : gtk_spin_button_get_value(GTK_SPIN_BUTTON(w));
}

static void on_apply(GtkButton *btn, gpointer data)
{
  (void) btn;
  PresetsPage *p = data;
  for (int r = 0; r < POX_N_PRESETS; r++) {
    PoxTune t;
    pox_io_load_preset(pox_preset_names[r], &t);
    for (int c = 0; c < N_FIELDS; c++)
      tune_set(&t, (Field) c, widget_get(p->w[r][c], &cols[c]));
    pox_io_save_preset(pox_preset_names[r], &t);
  }
  pox_io_reconfigure();
  if (p->cb)
    p->cb(p->cb_data, "Presets saved");
}

GtkWidget *
pox_presets_page_new(PoxSavedCb cb, gpointer cb_data)
{
  PresetsPage *p = g_new0(PresetsPage, 1);
  p->cb = cb;
  p->cb_data = cb_data;

  GtkWidget *grid = gtk_grid_new();
  gtk_widget_add_css_class(grid, "preset-row");
  gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);   /* float the table in the window */
  gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_widget_set_margin_top(grid, 14);
  gtk_widget_set_margin_bottom(grid, 14);
  gtk_widget_set_margin_start(grid, 14);
  gtk_widget_set_margin_end(grid, 14);

  for (int c = 0; c < N_FIELDS; c++) {
    GtkWidget *h = gtk_label_new(cols[c].header);
    gtk_widget_add_css_class(h, "dim-label");
    gtk_widget_add_css_class(h, "tune-header");
    if (cols[c].tooltip)
      gtk_widget_set_tooltip_text(h, cols[c].tooltip);
    gtk_grid_attach(GTK_GRID(grid), h, c + 1, 0, 1, 1);
  }

  for (int r = 0; r < POX_N_PRESETS; r++) {
    PoxTune t;
    pox_io_load_preset(pox_preset_names[r], &t);

    GtkWidget *name = gtk_label_new(pox_preset_names[r]);
    char *cls = g_strconcat("preset-", pox_preset_names[r], NULL);
    gtk_widget_add_css_class(name, cls);
    g_free(cls);
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), name, 0, r + 1, 1, 1);

    for (int c = 0; c < N_FIELDS; c++) {
      const ColDesc *cd = &cols[c];
      GtkWidget *w;
      if (cd->is_enum) {
        w = pox_cycle_new(cd->glyphs, cd->n_glyphs, cd->base,
                          (int) tune_get(&t, (Field) c), cd->css, cd->tooltip);
      } else {
        w = pox_spin_new(cd->lo, cd->hi, cd->step, cd->digits,
                         tune_get(&t, (Field) c), FALSE, cd->tooltip);
        gtk_widget_set_size_request(w, 58, -1);
      }
      gtk_widget_set_halign(w, GTK_ALIGN_FILL);
      p->w[r][c] = w;
      gtk_grid_attach(GTK_GRID(grid), w, c + 1, r + 1, 1, 1);
    }
  }

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), grid);
  gtk_widget_set_vexpand(scroller, TRUE);

  GtkWidget *apply = gtk_button_new_with_label("Apply");
  gtk_widget_add_css_class(apply, "suggested-action");
  g_signal_connect(apply, "clicked", G_CALLBACK(on_apply), p);
  GtkWidget *actionbar = gtk_action_bar_new();
  gtk_action_bar_pack_end(GTK_ACTION_BAR(actionbar), apply);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), scroller);
  gtk_box_append(GTK_BOX(box), actionbar);

  g_object_set_data_full(G_OBJECT(box), "pox-presets-page", p, g_free);
  return box;
}
