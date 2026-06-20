# Standalone GTK4 / libadwaita configurator for poxicle integrations: edits preset
# tunables and per-app rules and asks the compositor to reload them. Toolkit-only
# and independent of both the engine and the KWin effect, so it builds wherever
# gtk4 + libadwaita are present. It currently targets the KWin effect's kwinrc keys
# (via kreadconfig6 / kwriteconfig6 at runtime); a future GNOME integration can
# share this same UI.
gtk4_dep    = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1')

executable('poxicle-config',
  'main.c',
  'pox-io.c',
  'pox-cells.c',
  'pox-presets-page.c',
  'pox-apps-page.c',
  dependencies : [gtk4_dep, adwaita_dep],
  install : true)
