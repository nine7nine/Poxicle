/* main.c — standalone GTK4/libadwaita Poxicle configurator.
 *
 * A dedicated app (Chiguiro's stack) for the same kwinrc [Effect-poxicle_kwin]
 * config the KWin effect + KCM use: a Presets page (per-preset tunables — the
 * editor the KCM lacks) and an Apps page (per-app rules). Saving writes kwinrc
 * and asks KWin to reload the effect live.
 */
#include "pox-config.h"
#include "pox-cells.h"

static void
on_saved (gpointer user_data, const char *msg)
{
  AdwToastOverlay *overlay = ADW_TOAST_OVERLAY (user_data);
  adw_toast_overlay_add_toast (overlay, adw_toast_new (msg));
}

static void
activate (GtkApplication *app, gpointer user_data)
{
  pox_load_css ();

  GtkWidget *win = adw_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (win), "Poxicle Particles");
  gtk_window_set_default_size (GTK_WINDOW (win), 940, 640);

  GtkWidget *overlay = adw_toast_overlay_new ();

  AdwViewStack *stack = ADW_VIEW_STACK (adw_view_stack_new ());
  adw_view_stack_add_titled_with_icon (stack, pox_presets_page_new (on_saved, overlay),
                                        "presets", "Presets", "applications-graphics-symbolic");
  adw_view_stack_add_titled_with_icon (stack, pox_apps_page_new (on_saved, overlay),
                                        "apps", "Apps", "view-list-symbolic");
  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (overlay), GTK_WIDGET (stack));

  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), stack);
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);

  GtkWidget *toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), overlay);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), toolbar);
  gtk_window_present (GTK_WINDOW (win));
}

int
main (int argc, char **argv)
{
  AdwApplication *app = adw_application_new ("org.ninez.PoxicleConfig",
                                             G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
