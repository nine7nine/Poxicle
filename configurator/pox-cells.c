/* pox-cells.c — see pox-cells.h. CSS + cell widgets ported from Chiguiro
 * (kgx-settings-page.c + style.css), rebased onto a `.preset-row` ancestor. */
#include "pox-cells.h"
#include "pox-config.h"   /* PoxAppearance */

#include <poxicle.h>   /* pox_palette_* — authoritative built-in palette data */
#include <stdio.h>     /* sscanf — parse #rrggbb glass colour */

static const char *CSS =
  /* ---- app-glass: flat dark translucent plane, forced-dark, own accent ----
   * The window's background alpha is composited by KWin (same trick as Chiguiro
   * and WeazyStroke), giving the glass plane. Redefining the accent named-colours
   * stops the system theme from bleeding its own accent/text colours in. */
  "@define-color accent_bg_color #3584e4;"
  "@define-color accent_color #3584e4;"
  "@define-color accent_fg_color #ffffff;"
  "window { background-color: rgba(20,20,26,0.90); color: rgba(255,255,255,0.97); }"
  "headerbar, .toolbar { background: transparent; background-image: none;"
  "  box-shadow: none; border: none; }"
  /* Everything stacked on the window is transparent so the single glass tint on
   * the window node shows through uniformly. NB: do NOT transparent-out
   * `.background` — it is the window's own background node (and outranks the
   * `window` selector), so doing so would zero the tint to fully see-through. */
  "box, grid, stack, viewstack, list, row, scrolledwindow, viewport, .view,"
  "toolbarview { background: transparent; background-image: none; }"
  "label { text-shadow: 0 1px 2px rgba(0,0,0,0.55); }"
  "separator { background-color: rgba(255,255,255,0.10); }"
  /* view-switcher tabs */
  "viewswitcher button { background: transparent; box-shadow: none;"
  "  color: rgba(255,255,255,0.82); }"
  "viewswitcher button:hover { background-color: rgba(255,255,255,0.06); }"
  /* Active tab: LIGHT text (readable focused and unfocused), accent shown via the
   * tint + border. Accent-coloured text went dark/unreadable on backdrop. */
  "viewswitcher button:checked { color: #ffffff;"
  "  background-color: color-mix(in srgb, @accent_bg_color 24%, transparent);"
  "  border: 1px solid color-mix(in srgb, @accent_bg_color 70%, transparent); }"
  "viewswitcher button:backdrop { color: rgba(255,255,255,0.80); }"
  "viewswitcher button:checked:backdrop { color: #ffffff;"
  "  background-color: color-mix(in srgb, @accent_bg_color 28%, transparent);"
  "  border-color: color-mix(in srgb, @accent_bg_color 62%, transparent); }"
  /* normal (non-cell) buttons read as glass — but NEVER the window-decoration
   * buttons (close/minimize/maximize/titlebutton): leave those to the theme,
   * exactly as Chiguiro / WeazyStroke do. Deliberately set NO text colour here:
   * normal buttons inherit the light window colour, while the per-column cell
   * glyphs keep their own `.tune-*` colours (this rule's :not() chain otherwise
   * outranks them and would flatten every glyph to one washed-out white). */
  "button:not(.titlebutton):not(.close):not(.minimize):not(.maximize) {"
  "  background: transparent; background-image: none; box-shadow: none;"
  "  border: 1px solid rgba(255,255,255,0.15); border-radius: 6px; }"
  "button:not(.titlebutton):not(.close):not(.minimize):not(.maximize):hover {"
  "  border-color: rgba(255,255,255,0.30);"
  "  background-color: rgba(255,255,255,0.05); }"
  "button.suggested-action {"
  "  border-color: color-mix(in srgb, @accent_bg_color 75%, transparent);"
  "  color: @accent_color; }"
  /* dropdowns / entries outside the .preset-row cells */
  "dropdown > button, entry, combobox button {"
  "  background: transparent; border: 1px solid rgba(255,255,255,0.15);"
  "  border-radius: 6px; color: rgba(255,255,255,0.97); }"
  "entry > text { color: rgba(255,255,255,0.97); caret-color: rgba(255,255,255,0.97); }"
  "entry > text > placeholder { color: rgba(255,255,255,0.40); }"
  /* lists */
  "list, list > row { background: transparent; }"
  "list > row:selected {"
  "  background-color: color-mix(in srgb, @accent_bg_color 20%, transparent); }"
  /* A selected row must NOT fall to GTK's dark selected-foreground: force EVERY
   * descendant (labels, dropdown/button labels, spin text) to light. The `*`
   * out-specifies the per-cell colour rules so nothing stays dark on selection. */
  "list > row:selected, list > row:selected *,"
  "list > row:selected label, list > row:selected text {"
  "  color: rgba(255,255,255,0.97); }"
  "list > row:selected:backdrop, list > row:selected:backdrop *,"
  "list > row:selected:backdrop label, list > row:selected:backdrop text {"
  "  color: rgba(255,255,255,0.92); }"
  /* checkbuttons use our accent */
  "checkbutton check:checked, check:checked { background-color: @accent_bg_color;"
  "  background-image: none; border-color: @accent_bg_color; color: #ffffff; }"
  /* popovers MUST be opaque — the window alpha would otherwise show through */
  "popover > contents { background-color: rgb(34,34,42);"
  "  border: 1px solid rgba(255,255,255,0.14);"
  "  box-shadow: 0 6px 18px rgba(0,0,0,0.55); color: rgba(255,255,255,0.97); }"
  "popover > arrow { background-color: rgb(34,34,42);"
  "  border: 1px solid rgba(255,255,255,0.14); }"
  "popover, popover label, popover row { color: rgba(255,255,255,0.97); }"
  "popover row:selected, popover row:hover {"
  "  background-color: color-mix(in srgb, @accent_bg_color 50%, transparent); }"
  "toast { color: rgba(255,255,255,0.97); }"
  /* scrollbars */
  "scrollbar, scrollbar > trough { background: transparent; border: none; }"
  "scrollbar > range > trough > slider { background-color: rgba(255,255,255,0.25); }"
  /* compact spin: thin border, transparent, +/- chrome hidden */
  ".preset-row spinbutton {"
  "  background: transparent;"
  "  border: 1px solid rgba(255,255,255,0.15);"
  "  border-radius: 3px; min-height: 0; min-width: 0; padding: 0;"
  "  color: rgba(255,255,255,0.95); }"
  ".preset-row spinbutton:focus-within { border-color: rgba(255,255,255,0.35); }"
  ".preset-row spinbutton button {"
  "  all: unset; min-width: 0; min-height: 0; padding: 0; margin: 0;"
  "  border: none; background: none; opacity: 0; -gtk-icon-size: 1px; }"
  ".preset-row spinbutton text {"
  "  min-height: 0; padding: 2px 6px; caret-color: currentColor; color: inherit; }"
  /* flat cycle buttons */
  ".preset-row button.flat {"
  "  border: 1px solid rgba(255,255,255,0.15); border-radius: 3px;"
  "  min-height: 0; min-width: 0; padding: 2px 6px; }"
  ".tune-header { font-size: small; }"
  /* Readability: never let text go dark-grey. libadwaita's .dim-label drops to
   * ~0.55 opacity, which is unreadable on the dark glass (worse when the window
   * is unfocused and the toolkit dims the backdrop too). Pin every dim/header
   * label to a light tone at full opacity, focused AND backdrop. */
  "label.dim-label, .dim-label {"
  "  opacity: 1; color: rgba(255,255,255,0.72); }"
  "label.dim-label:backdrop, .dim-label:backdrop, .tune-header:backdrop {"
  "  opacity: 1; color: rgba(255,255,255,0.66); }"
  "window:backdrop, headerbar:backdrop, .toolbar:backdrop {"
  "  color: rgba(255,255,255,0.95); }"
  /* per-preset name colours */
  ".preset-ambient   { color: #7cb8d9; }"
  ".preset-corners   { color: #60c890; }"
  ".preset-fireworks { color: #e0a050; }"
  ".preset-ping-pong { color: #d07080; }"
  ".preset-pulse-out { color: #c080d8; }"
  ".preset-rotate    { color: #58b8d8; }"
  ".preset-laser     { color: #ff5a5a; }"
  ".preset-tracer    { color: #5ad1ff; }"
  ".preset-comet     { color: #c8a0ff; }"
  ".preset-spinner   { color: #6fe0a0; }"
  ".preset-ripple    { color: #7fb0ff; }"
  ".preset-charge    { color: #ffc14d; }"
  ".preset-spread       { color: #ff7a40; }"
  ".preset-radar        { color: #50e0b0; }"
  ".preset-counter-spin { color: #a0d860; }"
  ".preset-snake        { color: #74cf6a; }"
  ".preset-breathe      { color: #9aa0ec; }"
  ".preset-strobe       { color: #e8e060; }"
  ".preset-fireflies    { color: #ffe38a; }"
  /* per-column cycle-button colours */
  "button.tune-gap     { color: #50d070; }"
  "button.tune-rls     { color: #e0a040; }"
  "button.tune-shape   { color: #60b0e0; }"
  "button.tune-curve   { color: #d070b0; }"
  "button.tune-thk-rls { color: #d0d050; }";

void
pox_load_css (void)
{
  static gboolean done = FALSE;
  if (done)
    return;
  done = TRUE;

  GtkCssProvider *p = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (p, CSS);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (p),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (p);
}

void
pox_apply_appearance (const PoxAppearance *a)
{
  /* A second provider at PRIORITY_USER (above the static APPLICATION sheet). We
   * only redefine the accent named-colours + the window glass: every static rule
   * that references @accent_bg_color / @accent_color (checkbuttons, suggested
   * actions, the active tab, selections, focus) re-resolves to the new colour for
   * free, so nothing else needs restating here. */
  static GtkCssProvider *dyn = NULL;
  if (!dyn) {
    dyn = gtk_css_provider_new ();
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (dyn),
                                                GTK_STYLE_PROVIDER_PRIORITY_USER);
  }

  unsigned int r = 20, g = 20, b = 26;
  if (a->glass[0] == '#')
    sscanf (a->glass + 1, "%2x%2x%2x", &r, &g, &b);
  double alpha = CLAMP (a->opacity, 0, 100) / 100.0;
  const char *ac = a->accent;

  char *css = g_strdup_printf (
    "@define-color accent_bg_color %s;"
    "@define-color accent_color %s;"
    "@define-color accent_fg_color #ffffff;"
    "window { background-color: rgba(%u,%u,%u,%.3f); }",
    ac, ac, r, g, b, alpha);
  gtk_css_provider_load_from_string (dyn, css);
  g_free (css);
}

/* ---- compact spin ---- */

static gboolean
spin_dash_output (GtkSpinButton *spin, gpointer user_data)
{
  (void) user_data;
  if (gtk_spin_button_get_value (spin) == 0.0) {
    gtk_editable_set_text (GTK_EDITABLE (spin), "—");   /* — */
    return TRUE;
  }
  return FALSE;
}

GtkWidget *
pox_spin_new (double lo, double hi, double step, int digits,
              double value, gboolean dash_zero, const char *tooltip)
{
  GtkWidget *s = gtk_spin_button_new_with_range (lo, hi, step);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (s), digits);
  gtk_editable_set_alignment (GTK_EDITABLE (s), 1.0);
  if (dash_zero)
    g_signal_connect (s, "output", G_CALLBACK (spin_dash_output), NULL);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (s), value);
  if (tooltip)
    gtk_widget_set_tooltip_text (s, tooltip);
  return s;
}

/* ---- glyph cycle button ---- */

static void
cycle_clicked (GtkButton *b, gpointer user_data)
{
  (void) user_data;
  int n    = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "n"));
  int base = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "base"));
  int val  = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "val"));
  if (n < 2)
    return;
  pox_cycle_set_value (GTK_WIDGET (b), base + ((val - base + 1) % n));
}

GtkWidget *
pox_cycle_new (const char *const *labels, int n, int base,
               int value, const char *css, const char *tooltip)
{
  GtkWidget *b = gtk_button_new ();
  gtk_widget_add_css_class (b, "flat");
  if (css)
    gtk_widget_add_css_class (b, css);
  g_object_set_data (G_OBJECT (b), "labels", (gpointer) labels);
  g_object_set_data (G_OBJECT (b), "n", GINT_TO_POINTER (n));
  g_object_set_data (G_OBJECT (b), "base", GINT_TO_POINTER (base));
  pox_cycle_set_value (b, value);
  g_signal_connect (b, "clicked", G_CALLBACK (cycle_clicked), NULL);
  if (tooltip)
    gtk_widget_set_tooltip_text (b, tooltip);
  return b;
}

void
pox_cycle_set_value (GtkWidget *btn, int value)
{
  int n    = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "n"));
  int base = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "base"));
  const char *const *labels = g_object_get_data (G_OBJECT (btn), "labels");
  int idx = CLAMP (value - base, 0, n - 1);
  g_object_set_data (G_OBJECT (btn), "val", GINT_TO_POINTER (base + idx));
  gtk_button_set_label (GTK_BUTTON (btn), labels[idx]);
}

int
pox_cycle_value (GtkWidget *btn)
{
  return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "val"));
}

/* ---- palette dropdown ---- */
/* Dropdown rows map position -> stored value as (position - 1): row 0 is the
 * "Solid" entry (value -1, use the per-app colour), rows 1..N are built-in
 * palette ids 0..N-1. A custom factory previews each as a swatch strip + name. */

/* "#rrggbb" markup of palette `id`'s colours as filled squares, or NULL for the
 * Solid row (no preview). Caller frees. */
static char *
pal_swatch_markup (guint pos)
{
  if (pos == 0)
    return NULL;   /* Solid: no preview strip */
  PoxColor cs[POX_PALETTE_MAX];
  int n = pox_palette_colors ((int) pos - 1, cs, POX_PALETTE_MAX);
  GString *s = g_string_new (NULL);
  for (int i = 0; i < n; i++)
    g_string_append_printf (s, "<span foreground=\"#%02x%02x%02x\">\xe2\x96\xa0</span>",
                            (int) (cs[i].r * 255 + 0.5f),
                            (int) (cs[i].g * 255 + 0.5f),
                            (int) (cs[i].b * 255 + 0.5f));
  return g_string_free (s, FALSE);
}

static const char *
pal_name (guint pos)
{
  if (pos == 0)
    return "Solid";
  const char *n = pox_palette_name ((int) pos - 1);
  return n ? n : "?";
}

static void
pal_setup (GtkSignalListItemFactory *f, GtkListItem *item, gpointer u)
{
  (void) f; (void) u;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *sw  = gtk_label_new (NULL);   /* swatch strip (markup) */
  GtkWidget *nm  = gtk_label_new (NULL);   /* palette name */
  gtk_widget_set_halign (nm, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), sw);
  gtk_box_append (GTK_BOX (box), nm);
  gtk_list_item_set_child (item, box);
}

static void
pal_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer u)
{
  (void) f; (void) u;
  guint pos = gtk_list_item_get_position (item);
  GtkWidget *box = gtk_list_item_get_child (item);
  GtkWidget *sw  = gtk_widget_get_first_child (box);
  GtkWidget *nm  = gtk_widget_get_next_sibling (sw);
  char *m = pal_swatch_markup (pos);
  gtk_label_set_markup (GTK_LABEL (sw), m ? m : "");
  gtk_widget_set_visible (sw, m != NULL);
  g_free (m);
  gtk_label_set_text (GTK_LABEL (nm), pal_name (pos));
}

GtkWidget *
pox_palette_new (int value, const char *tooltip)
{
  /* Model length = Solid + every built-in palette. The factory renders by
   * position, so the strings are just placeholders that set the row count. */
  int count = pox_palette_count ();
  const char **names = g_new0 (const char *, count + 2);
  names[0] = "Solid";
  for (int i = 0; i < count; i++)
    names[i + 1] = pox_palette_name (i);
  GtkStringList *model = gtk_string_list_new (names);
  g_free (names);   /* gtk_string_list_new copies the strings */

  GtkListItemFactory *fac = gtk_signal_list_item_factory_new ();
  g_signal_connect (fac, "setup", G_CALLBACK (pal_setup), NULL);
  g_signal_connect (fac, "bind",  G_CALLBACK (pal_bind),  NULL);

  GtkWidget *dd = gtk_drop_down_new (G_LIST_MODEL (model), NULL);
  gtk_drop_down_set_factory (GTK_DROP_DOWN (dd), fac);
  g_object_unref (fac);
  gtk_widget_add_css_class (dd, "flat");
  pox_palette_set_value (dd, value);
  if (tooltip)
    gtk_widget_set_tooltip_text (dd, tooltip);
  return dd;
}

int
pox_palette_value (GtkWidget *dd)
{
  guint pos = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
  if (pos == GTK_INVALID_LIST_POSITION)
    return 0;
  return (int) pos - 1;   /* row 0 = Solid (-1) */
}

void
pox_palette_set_value (GtkWidget *dd, int value)
{
  GListModel *m = gtk_drop_down_get_model (GTK_DROP_DOWN (dd));
  guint n = m ? g_list_model_get_n_items (m) : 0;
  int pos = value + 1;                 /* -1 (Solid) -> 0 */
  if (pos < 0 || (guint) pos >= n)
    pos = 1;                           /* fall back to the first palette (Muted) */
  gtk_drop_down_set_selected (GTK_DROP_DOWN (dd), (guint) pos);
}

/* ---- plain text dropdown (preset picker) ---- */

GtkWidget *
pox_preset_new (const char *const *labels, int n, int value, const char *tooltip)
{
  GtkStringList *model = gtk_string_list_new (NULL);
  for (int i = 0; i < n; i++)
    gtk_string_list_append (model, labels[i]);

  GtkWidget *dd = gtk_drop_down_new (G_LIST_MODEL (model), NULL);
  gtk_widget_add_css_class (dd, "flat");
  pox_preset_set_value (dd, value);
  if (tooltip)
    gtk_widget_set_tooltip_text (dd, tooltip);
  return dd;
}

int
pox_preset_value (GtkWidget *dd)
{
  guint pos = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
  return (pos == GTK_INVALID_LIST_POSITION) ? 0 : (int) pos;
}

void
pox_preset_set_value (GtkWidget *dd, int value)
{
  GListModel *m = gtk_drop_down_get_model (GTK_DROP_DOWN (dd));
  guint n = m ? g_list_model_get_n_items (m) : 0;
  if (value < 0 || (guint) value >= n)
    value = 0;
  gtk_drop_down_set_selected (GTK_DROP_DOWN (dd), (guint) value);
}
