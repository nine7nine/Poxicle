/* pox-cells.h — compact grid cells lifted from Chiguiro's settings page:
 * a CSS-slimmed spin (no +/- chrome) and a flat single-glyph cycle button.
 * Both rely on a `.preset-row` ancestor for the compact styling. */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* Install the app stylesheet on the default display (idempotent). */
void pox_load_css (void);

/* Right-aligned numeric cell; +/- buttons hidden by CSS (edit by typing/scroll).
 * dash_zero => render the value 0 as "—" (an "inherit" sentinel). */
GtkWidget *pox_spin_new (double lo, double hi, double step, int digits,
                         double value, gboolean dash_zero, const char *tooltip);

/* Flat button that cycles `n` glyph labels on click; the stored value is
 * base..base+n-1. `css` is an extra style class (per-column colour) or NULL. */
GtkWidget *pox_cycle_new (const char *const *labels, int n, int base,
                          int value, const char *css, const char *tooltip);
int  pox_cycle_value     (GtkWidget *btn);
void pox_cycle_set_value (GtkWidget *btn, int value);

/* Flat dropdown of `n` text labels (e.g. preset names) — a real pick-from-a-list
 * menu instead of click-through cycling. Stored value is the selected index
 * 0..n-1. `labels` must outlive the call only (the strings are copied). */
GtkWidget *pox_preset_new       (const char *const *labels, int n,
                                  int value, const char *tooltip);
int        pox_preset_value     (GtkWidget *dd);
void       pox_preset_set_value (GtkWidget *dd, int value);

/* Dropdown of the engine's built-in burst palettes (pox_palette_* in poxicle.h),
 * each previewed as a coloured swatch strip + name, with a leading "Solid" entry.
 * Stored value: a built-in palette id (0..pox_palette_count()-1), or -1 = "Solid"
 * (sample the per-app colour). */
GtkWidget *pox_palette_new       (int value, const char *tooltip);
int        pox_palette_value     (GtkWidget *dd);
void       pox_palette_set_value (GtkWidget *dd, int value);

G_END_DECLS
