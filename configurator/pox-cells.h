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

G_END_DECLS
