/* pox-cells.c — see pox-cells.h. CSS + cell widgets ported from Chiguiro
 * (kgx-settings-page.c + style.css), rebased onto a `.preset-row` ancestor. */
#include "pox-cells.h"

static const char *CSS =
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
  /* per-preset name colours */
  ".preset-ambient   { color: #7cb8d9; }"
  ".preset-corners   { color: #60c890; }"
  ".preset-fireworks { color: #e0a050; }"
  ".preset-ping-pong { color: #d07080; }"
  ".preset-pulse-out { color: #c080d8; }"
  ".preset-rotate    { color: #58b8d8; }"
  ".preset-scroll2   { color: #78a5d4; }"
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
