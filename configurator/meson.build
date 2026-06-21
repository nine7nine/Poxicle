# Standalone GTK4 / libadwaita configurator for poxicle integrations: edits preset
# tunables and per-app rules and asks the compositor to reload them. Toolkit-only;
# it builds wherever gtk4 + libadwaita are present. It targets the KWin effect's
# kwinrc keys (via kreadconfig6 / kwriteconfig6 at runtime); a future GNOME
# integration can share this same UI.
#
# It also compiles the engine's *simulation* unit (../src/poxicle.c) — which is
# pure C with no GL/Wayland/toolkit deps — purely for the built-in palette table
# (pox_palette_*), so the palette dropdown lists the same names/colours the effect
# renders, with zero duplication. The GL backend (poxicle-gl.c) is NOT linked.
gtk4_dep    = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1')
m_dep       = meson.get_compiler('c').find_library('m', required : false)  # poxicle.c: sqrtf

executable('poxicle-config',
  'main.c',
  'pox-io.c',
  'pox-cells.c',
  'pox-presets-page.c',
  'pox-apps-page.c',
  'pox-prefs-page.c',
  '../src/poxicle.c',
  include_directories : include_directories('../include'),
  dependencies : [gtk4_dep, adwaita_dep, m_dep],
  install : true)

# Desktop entry + app icon, so the configurator shows a name/icon in launchers and
# the compositor maps its Wayland app_id (org.ninez.PoxicleConfig) to this icon.
install_data('org.ninez.PoxicleConfig.desktop',
  install_dir : get_option('datadir') / 'applications')
install_data('org.ninez.PoxicleConfig.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
