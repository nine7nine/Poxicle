/* pox-prefs-page.c — Preferences page: the configurator's OWN window appearance
 * (glass tint / window opacity / accent colour), in the WeazyStroke / easystroke
 * style. Each control applies live (pox_apply_appearance) and self-persists
 * (pox_io_save_appearance) — no Apply button. */
#include "pox-config.h"

typedef struct {
  PoxAppearance ap;
  GtkWidget    *opacity, *glass, *accent;
} PrefsPage;

static char *
rgba_to_hex (const GdkRGBA *c)
{
  return g_strdup_printf ("#%02x%02x%02x",
                          (int) (c->red   * 255.0 + 0.5),
                          (int) (c->green * 255.0 + 0.5),
                          (int) (c->blue  * 255.0 + 0.5));
}

static void
push (PrefsPage *p)
{
  pox_apply_appearance (&p->ap);
  pox_io_save_appearance (&p->ap);
}

static void
on_opacity (GtkSpinButton *sp, gpointer data)
{
  PrefsPage *p = data;
  p->ap.opacity = (int) gtk_spin_button_get_value (sp);
  push (p);
}

static void
on_glass (GObject *btn, GParamSpec *ps, gpointer data)
{
  (void) ps;
  PrefsPage *p = data;
  char *hex = rgba_to_hex (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (btn)));
  g_strlcpy (p->ap.glass, hex, sizeof p->ap.glass);
  g_free (hex);
  push (p);
}

static void
on_accent (GObject *btn, GParamSpec *ps, gpointer data)
{
  (void) ps;
  PrefsPage *p = data;
  char *hex = rgba_to_hex (gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (btn)));
  g_strlcpy (p->ap.accent, hex, sizeof p->ap.accent);
  g_free (hex);
  push (p);
}

/* The un-minimize grace is an EFFECT setting (poxicle group), not GUI styling:
 * save it and ask the compositor to reload (the GNOME extension picks it up via
 * its config-file monitor). */
static void
on_grace (GtkSpinButton *sp, gpointer data)
{
  (void) data;
  pox_io_save_grace ((int) gtk_spin_button_get_value (sp));
  pox_io_reconfigure ();
}

static GtkWidget *
color_btn (const char *hex, GCallback cb, gpointer data)
{
  GtkWidget *btn = gtk_color_dialog_button_new (gtk_color_dialog_new ());
  GdkRGBA rgba;
  if (gdk_rgba_parse (&rgba, hex))
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (btn), &rgba);
  gtk_widget_set_halign (btn, GTK_ALIGN_START);
  gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
  g_signal_connect (btn, "notify::rgba", cb, data);
  return btn;
}

static void
add_row (GtkGrid *grid, int row, const char *label, GtkWidget *ctrl)
{
  GtkWidget *l = gtk_label_new (label);
  gtk_widget_set_halign (l, GTK_ALIGN_START);
  gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
  gtk_grid_attach (grid, l, 0, row, 1, 1);
  gtk_grid_attach (grid, ctrl, 1, row, 1, 1);
}

GtkWidget *
pox_prefs_page_new (void)
{
  PrefsPage *p = g_new0 (PrefsPage, 1);
  pox_io_load_appearance (&p->ap);

  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 14);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 22);
  gtk_widget_set_halign (grid, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (grid, GTK_ALIGN_START);
  gtk_widget_set_margin_top (grid, 30);
  gtk_widget_set_margin_bottom (grid, 18);
  gtk_widget_set_margin_start (grid, 18);
  gtk_widget_set_margin_end (grid, 18);

  GtkWidget *hdr = gtk_label_new ("Appearance");
  gtk_widget_add_css_class (hdr, "title-4");
  gtk_widget_set_halign (hdr, GTK_ALIGN_START);
  gtk_widget_set_margin_bottom (hdr, 4);
  gtk_grid_attach (GTK_GRID (grid), hdr, 0, 0, 2, 1);

  p->opacity = gtk_spin_button_new_with_range (0, 100, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (p->opacity), p->ap.opacity);
  gtk_widget_set_halign (p->opacity, GTK_ALIGN_START);
  g_signal_connect (p->opacity, "value-changed", G_CALLBACK (on_opacity), p);
  add_row (GTK_GRID (grid), 1, "Window Opacity", p->opacity);

  p->glass = color_btn (p->ap.glass, G_CALLBACK (on_glass), p);
  add_row (GTK_GRID (grid), 2, "Glass Color", p->glass);

  p->accent = color_btn (p->ap.accent, G_CALLBACK (on_accent), p);
  add_row (GTK_GRID (grid), 3, "Accent Color", p->accent);

  GtkWidget *bhdr = gtk_label_new ("Behavior");
  gtk_widget_add_css_class (bhdr, "title-4");
  gtk_widget_set_halign (bhdr, GTK_ALIGN_START);
  gtk_widget_set_margin_top (bhdr, 14);
  gtk_widget_set_margin_bottom (bhdr, 4);
  gtk_grid_attach (GTK_GRID (grid), bhdr, 0, 4, 2, 1);

  GtkWidget *grace = gtk_spin_button_new_with_range (0, 2000, 25);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (grace), pox_io_load_grace ());
  gtk_widget_set_halign (grace, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text (grace,
    "How long streamed particles are held back after a window starts "
    "un-minimizing, so the ring doesn't snap in over a still-animating window "
    "(Magic Lamp / Burn My Windows). Scaled by your animation speed.");
  g_signal_connect (grace, "value-changed", G_CALLBACK (on_grace), p);
  add_row (GTK_GRID (grid), 5, "Un-minimize grace (ms)", grace);

  g_object_set_data_full (G_OBJECT (grid), "pox-prefs-page", p, g_free);
  return grid;
}
