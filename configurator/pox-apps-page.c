/* pox-apps-page.c — per-app rules editor, in Chiguiro's compact App-Glass style:
 * cycle buttons for preset/reverse/shape/gap/release, slim dash-spins for the
 * numeric overrides. Apply writes the Rules StringList + DefaultPreset. */
#include "pox-config.h"
#include "pox-cells.h"

/* Preset choices for a rule (includes "none" = off), index == cycle value. */
static const char *const apps_presets[] = {
  "ambient", "none", "corners", "fireworks", "ping-pong", "pulse-out",
  "rotate", "laser", "tracer", "comet", "spinner", "ripple", "charge",
  "spread", "radar", "counter-spin", "snake", "breathe", "strobe", "fireflies", NULL,
};
#define N_APPS_PRESETS 20

static const char *const g_rev[]   = { "▶", "◀", "◆" };                  /* fwd / rev / loop */
static const char *const g_shape[]  = { "—", "■", "●", "◆", "▶" };  /* base -1: inherit + shapes */
static const char *const g_gap[]    = { "—", "□", "■" };                         /* base -1: inherit / 0=gapped / 1=solid */
static const char *const g_rmode[]  = { "—", "U", "R", "S", "G", "A" };          /* base -1: inherit + modes */

#define N_COLS 16

typedef struct {
  GtkWidget *app, *preset, *rev, *color, *shape, *gap,
            *spd, *thk, *tail, *atk, *rel, *rls, *tatk, *trel, *trls, *palette;
  char      *color_hex;     /* "#rrggbb" or NULL = inherit */
  int        is_active;     /* the focus-following "Active window" row (no app id) */
} RuleRow;

typedef struct {
  PoxSavedCb    cb;
  gpointer      cb_data;
  GtkWidget    *list;
  GtkWidget    *default_preset;
  GtkSizeGroup *sg[N_COLS];
} AppsPage;

static void rule_row_free(gpointer p)
{
  RuleRow *r = p;
  g_free(r->color_hex);
  g_free(r);
}

static int name_index(const char *name)
{
  for (int i = 0; apps_presets[i]; i++)
    if (g_strcmp0(apps_presets[i], name) == 0) return i;
  return 0;
}

/* ---- colour (compact dot; hex in tooltip) ---- */

static void set_color_label(GtkWidget *btn, const char *hex)
{
  GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(btn));
  if (hex && *hex) {
    char *m = g_strdup_printf("<span foreground=\"%s\">●</span>", hex);
    gtk_label_set_markup(GTK_LABEL(lbl), m);
    g_free(m);
    gtk_widget_set_tooltip_text(btn, hex);
  } else {
    gtk_label_set_text(GTK_LABEL(lbl), "—");
    gtk_widget_set_tooltip_text(btn, "No per-app colour (inherit)");
  }
}

static void color_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
  RuleRow *row = data;
  GdkRGBA *rgba = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(src), res, NULL);
  if (!rgba)
    return;
  char hex[8];
  g_snprintf(hex, sizeof hex, "#%02x%02x%02x",
             (int) (rgba->red * 255 + 0.5), (int) (rgba->green * 255 + 0.5),
             (int) (rgba->blue * 255 + 0.5));
  g_free(row->color_hex);
  row->color_hex = g_strdup(hex);
  set_color_label(row->color, hex);
  gdk_rgba_free(rgba);
}

static void on_color_clicked(GtkButton *btn, gpointer data)
{
  RuleRow *row = data;
  GtkColorDialog *dlg = gtk_color_dialog_new();
  GdkRGBA init = { 0.55f, 0.78f, 1.0f, 1.0f };
  if (row->color_hex)
    gdk_rgba_parse(&init, row->color_hex);
  GtkWindow *win = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)));
  gtk_color_dialog_choose_rgba(dlg, win, &init, NULL, color_chosen, row);
  g_object_unref(dlg);
}

/* ---- row build / read ---- */

static void attach(AppsPage *ap, GtkWidget *box, int col, GtkWidget *w)
{
  if (ap->sg[col])
    gtk_size_group_add_widget(ap->sg[col], w);
  gtk_box_append(GTK_BOX(box), w);
}

static void add_row(AppsPage *ap, const PoxRule *init, gboolean is_active)
{
  RuleRow *r = g_new0(RuleRow, 1);
  r->is_active = is_active;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(box, "preset-row");   /* compact spin styling */

  if (is_active) {
    /* The active-window target has no app id: a fixed label sits in the App
     * column. It is saved to the "Active" key, not the Rules list. */
    GtkWidget *lbl = gtk_label_new("★ Active window");
    gtk_widget_add_css_class(lbl, "preset-name");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_size_request(lbl, 150, -1);
    gtk_widget_set_tooltip_text(lbl,
      "Drawn on whichever window currently has focus, on top of that window's "
      "own per-app particles. Set preset to “none” to disable.");
    attach(ap, box, 0, lbl);
  } else {
    r->app = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->app), "app id");
    gtk_widget_set_size_request(r->app, 150, -1);
    if (init && init->app_id)
      gtk_editable_set_text(GTK_EDITABLE(r->app), init->app_id);
    attach(ap, box, 0, r->app);
  }

  r->preset = pox_cycle_new(apps_presets, N_APPS_PRESETS, 0,
                            init ? name_index(init->preset) : 0, NULL, "Preset");
  attach(ap, box, 1, r->preset);

  r->rev = pox_cycle_new(g_rev, 3, 0, init ? init->reverse : 0, NULL,
                         "Direction (▶ forward, ◀ reverse, ◆ loop)");
  attach(ap, box, 2, r->rev);

  r->color = gtk_button_new();
  gtk_widget_add_css_class(r->color, "flat");
  gtk_button_set_child(GTK_BUTTON(r->color), gtk_label_new(NULL));
  r->color_hex = (init && init->color) ? g_strdup(init->color) : NULL;
  set_color_label(r->color, r->color_hex);
  g_signal_connect(r->color, "clicked", G_CALLBACK(on_color_clicked), r);
  attach(ap, box, 3, r->color);

  r->shape = pox_cycle_new(g_shape, 5, -1, init ? init->shape : -1, "tune-shape", "Shape (— inherits)");
  attach(ap, box, 4, r->shape);
  r->gap = pox_cycle_new(g_gap, 3, -1, init ? init->gap : -1, "tune-gap", "Gap (— inherits)");
  attach(ap, box, 5, r->gap);

  r->spd  = pox_spin_new(0, 800, 1, 0, init ? init->speed : 0,     TRUE, "Speed % (0 inherits)");
  attach(ap, box, 6, r->spd);
  r->thk  = pox_spin_new(0, 100, 1, 0, init ? init->thickness : 0, TRUE, "Thickness px (0 inherits)");
  attach(ap, box, 7, r->thk);
  r->tail = pox_spin_new(0, 1000, 1, 0, init ? init->tail : 0,     TRUE, "Tail % (0 inherits)");
  attach(ap, box, 8, r->tail);
  r->atk  = pox_spin_new(0, 100, 1, 0, init ? init->attack : 0,    TRUE, "Attack % (0 inherits)");
  attach(ap, box, 9, r->atk);
  r->rel  = pox_spin_new(0, 100, 1, 0, init ? init->release : 0,   TRUE, "Release % (0 inherits)");
  attach(ap, box, 10, r->rel);

  r->rls = pox_cycle_new(g_rmode, 6, -1, init ? init->release_mode : -1, "tune-rls", "Release mode (— inherits)");
  attach(ap, box, 11, r->rls);

  r->tatk = pox_spin_new(0, 100, 1, 0, init ? init->thk_attack : 0,  TRUE, "Thk attack % (0 inherits)");
  attach(ap, box, 12, r->tatk);
  r->trel = pox_spin_new(0, 100, 1, 0, init ? init->thk_release : 0, TRUE, "Thk release % (0 inherits)");
  attach(ap, box, 13, r->trel);

  r->trls = pox_cycle_new(g_rmode, 6, -1, init ? init->thk_release_mode : -1, "tune-thk-rls", "Thk release mode (— inherits)");
  attach(ap, box, 14, r->trls);

  r->palette = pox_palette_new(init ? init->palette : 0,
                               "Ambient / fireworks colour theme (Solid = use the Color swatch)");
  attach(ap, box, 15, r->palette);

  GtkWidget *lrow = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(lrow), box);
  g_object_set_data_full(G_OBJECT(lrow), "rulerow", r, rule_row_free);
  gtk_list_box_append(GTK_LIST_BOX(ap->list), lrow);
}

/* Read the preset/overrides cells (everything except the app id) into out. */
static void fill_rule_fields(RuleRow *r, PoxRule *out)
{
  out->preset  = g_strdup(apps_presets[pox_cycle_value(r->preset)]);
  out->reverse = pox_cycle_value(r->rev);
  out->color   = r->color_hex ? g_strdup(r->color_hex) : NULL;
  out->shape   = pox_cycle_value(r->shape);            /* base -1 → -1..3 */
  out->gap     = pox_cycle_value(r->gap);
  out->release_mode     = pox_cycle_value(r->rls);
  out->thk_release_mode = pox_cycle_value(r->trls);
  out->speed       = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->spd));
  out->thickness   = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->thk));
  out->tail        = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->tail));
  out->attack      = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->atk));
  out->release     = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->rel));
  out->thk_attack  = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->tatk));
  out->thk_release = (int) gtk_spin_button_get_value(GTK_SPIN_BUTTON(r->trel));
  out->palette     = pox_palette_value(r->palette);
}

static PoxRule *row_to_rule(RuleRow *r)
{
  char *app = g_strdup(gtk_editable_get_text(GTK_EDITABLE(r->app)));
  g_strstrip(app);
  if (!*app) { g_free(app); return NULL; }

  PoxRule *out = g_new0(PoxRule, 1);
  out->app_id = app;
  fill_rule_fields(r, out);
  return out;
}

/* The active-window row has no app id — saved to the "Active" key instead. */
static PoxRule *active_row_to_rule(RuleRow *r)
{
  PoxRule *out = g_new0(PoxRule, 1);
  fill_rule_fields(r, out);
  return out;
}

/* ---- actions ---- */

/* Seed a fresh rule row with the default preset (shared by Add app / Pick
 * window); app_id NULL => an empty App entry the user types into. */
static void seed_row(AppsPage *ap, const char *app_id)
{
  PoxRule seed = { 0 };
  seed.preset = (char *) apps_presets[pox_cycle_value(ap->default_preset)];
  seed.shape = seed.gap = seed.release_mode = seed.thk_release_mode = -1;
  seed.app_id = (char *) app_id;
  add_row(ap, &seed, FALSE);
}

static void on_add(GtkButton *btn, gpointer data)
{
  (void) btn;
  seed_row(data, NULL);
}

/* KWin's interactive picker returned a window's app id (or NULL if cancelled). */
static void on_picked(const char *app_id, gpointer data)
{
  AppsPage *ap = data;
  if (!app_id || !*app_id) {
    if (ap->cb)
      ap->cb(ap->cb_data, "No window picked");
    return;
  }
  seed_row(ap, app_id);
  if (ap->cb)
    ap->cb(ap->cb_data, "Added picked window — set its preset, then Apply");
}

static void on_pick(GtkButton *btn, gpointer data)
{
  (void) btn;
  pox_io_pick_window(on_picked, data);
}

static void on_remove(GtkButton *btn, gpointer data)
{
  (void) btn;
  AppsPage *ap = data;
  GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(ap->list));
  if (!sel)
    return;
  RuleRow *r = g_object_get_data(G_OBJECT(sel), "rulerow");
  if (r && r->is_active) {
    if (ap->cb)
      ap->cb(ap->cb_data, "The active-window entry can't be removed");
    return;
  }
  gtk_list_box_remove(GTK_LIST_BOX(ap->list), GTK_WIDGET(sel));
}

static void on_apply(GtkButton *btn, gpointer data)
{
  (void) btn;
  AppsPage *ap = data;
  GPtrArray *rules = g_ptr_array_new_with_free_func((GDestroyNotify) pox_rule_free);
  PoxRule *active = NULL;

  for (GtkWidget *child = gtk_widget_get_first_child(ap->list);
       child; child = gtk_widget_get_next_sibling(child)) {
    RuleRow *r = g_object_get_data(G_OBJECT(child), "rulerow");
    if (!r) continue;
    if (r->is_active) {
      pox_rule_free(active);            /* keep the single active row */
      active = active_row_to_rule(r);
      continue;
    }
    PoxRule *pr = row_to_rule(r);
    if (pr) g_ptr_array_add(rules, pr);
  }

  pox_io_save_rules(rules);
  g_ptr_array_unref(rules);
  if (active) {
    pox_io_save_active(active);
    pox_rule_free(active);
  }
  pox_io_save_default_preset(apps_presets[pox_cycle_value(ap->default_preset)]);
  pox_io_reconfigure();
  if (ap->cb)
    ap->cb(ap->cb_data, "Apps saved");
}

/* ---- page ---- */

GtkWidget *
pox_apps_page_new(PoxSavedCb cb, gpointer cb_data)
{
  AppsPage *ap = g_new0(AppsPage, 1);
  ap->cb = cb;
  ap->cb_data = cb_data;
  for (int c = 0; c < N_COLS; c++)
    ap->sg[c] = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

  ap->list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(ap->list), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(ap->list, "boxed-list");

  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(top, 12);
  gtk_widget_set_margin_start(top, 14);
  gtk_widget_set_margin_end(top, 14);
  gtk_box_append(GTK_BOX(top), gtk_label_new("New app preset:"));
  ap->default_preset = pox_cycle_new(apps_presets, N_APPS_PRESETS, 0,
                                     name_index(pox_io_load_default_preset()), NULL, NULL);
  gtk_box_append(GTK_BOX(top), ap->default_preset);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(top), spacer);
  GtkWidget *pick = gtk_button_new_with_label("Pick window");
  gtk_widget_set_tooltip_text(pick, "Click a window to capture its app id");
  GtkWidget *add = gtk_button_new_with_label("Add app");
  GtkWidget *rem = gtk_button_new_with_label("Remove selected");
  g_signal_connect(pick, "clicked", G_CALLBACK(on_pick), ap);
  g_signal_connect(add, "clicked", G_CALLBACK(on_add), ap);
  g_signal_connect(rem, "clicked", G_CALLBACK(on_remove), ap);
  gtk_box_append(GTK_BOX(top), pick);
  gtk_box_append(GTK_BOX(top), add);
  gtk_box_append(GTK_BOX(top), rem);

  static const char *const heads[N_COLS] = {
    "App", "Preset", "Rev", "Color", "Shape", "Gap", "Spd", "Thk", "Tail",
    "Atk", "Rel", "Rls", "TAtk", "TRel", "TRls", "Palette",
  };
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(header, 14);
  gtk_widget_set_margin_end(header, 14);
  for (int c = 0; c < N_COLS; c++) {
    GtkWidget *h = gtk_label_new(heads[c]);
    gtk_widget_add_css_class(h, "dim-label");
    gtk_widget_add_css_class(h, "tune-header");
    gtk_widget_set_halign(h, c == 0 ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
    attach(ap, header, c, h);
  }

  GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign(inner, GTK_ALIGN_CENTER);   /* float the table in the window */
  gtk_widget_set_valign(inner, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(inner, 6);
  gtk_widget_set_margin_bottom(inner, 12);
  gtk_widget_set_margin_start(inner, 14);
  gtk_widget_set_margin_end(inner, 14);
  gtk_box_append(GTK_BOX(inner), header);
  gtk_box_append(GTK_BOX(inner), ap->list);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), inner);
  gtk_widget_set_vexpand(scroller, TRUE);

  GtkWidget *apply = gtk_button_new_with_label("Apply");
  gtk_widget_add_css_class(apply, "suggested-action");
  g_signal_connect(apply, "clicked", G_CALLBACK(on_apply), ap);
  GtkWidget *actionbar = gtk_action_bar_new();
  gtk_action_bar_pack_end(GTK_ACTION_BAR(actionbar), apply);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), top);
  gtk_box_append(GTK_BOX(box), scroller);
  gtk_box_append(GTK_BOX(box), actionbar);

  /* The focus-following "Active window" target sits at the top, always present. */
  PoxRule *active = pox_io_load_active();
  add_row(ap, active, TRUE);
  pox_rule_free(active);

  GPtrArray *rules = pox_io_load_rules();
  for (guint i = 0; i < rules->len; i++)
    add_row(ap, g_ptr_array_index(rules, i), FALSE);
  g_ptr_array_unref(rules);

  g_object_set_data_full(G_OBJECT(box), "pox-apps-page", ap, g_free);
  return box;
}
