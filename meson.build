project('poxicle', 'c',
  version : '0.0.1',
  license : 'LGPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')

wayland_client = dependency('wayland-client')
wayland_egl    = dependency('wayland-egl')
egl            = dependency('egl')
gles           = dependency('glesv2')
libm           = cc.find_library('m', required : false)

# Generate xdg-shell client glue from the system protocol description.
wl_scanner    = find_program('wayland-scanner')
wl_protocols  = dependency('wayland-protocols')
wl_proto_dir  = wl_protocols.get_variable(pkgconfig : 'pkgdatadir')
xdg_shell_xml = wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'

xdg_shell_h = custom_target('xdg-shell-client-header',
  input  : xdg_shell_xml,
  output : 'xdg-shell-client-protocol.h',
  command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

xdg_shell_c = custom_target('xdg-shell-protocol-code',
  input  : xdg_shell_xml,
  output : 'xdg-shell-protocol.c',
  command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

inc = include_directories('include')

# The library: sim core (poxicle.c) + GLES3 renderer (poxicle-gl.c). Needs only
# a GL context — no Wayland/EGL/windowing here, so it stays embeddable anywhere.
libpoxicle = static_library('poxicle',
  'src/poxicle.c',
  'src/poxicle-gl.c',
  include_directories : inc,
  dependencies : [gles, libm],
  install : false)

# Opt-in Wayland subsurface backend: rides poxicle on a host app's window.
# Depends on the windowing stack, kept out of the core library.
libpoxicle_wl = static_library('poxicle-wl',
  'src/poxicle-wl.c',
  include_directories : inc,
  dependencies : [wayland_client, wayland_egl, egl, gles],
  link_with : libpoxicle,
  install : false)

# Consumable dependencies for embedders (e.g. Chiguiro as a meson subproject).
poxicle_dep = declare_dependency(
  include_directories : inc,
  link_with : libpoxicle,
  dependencies : [gles, libm])
poxicle_wl_dep = declare_dependency(
  include_directories : inc,
  link_with : [libpoxicle_wl, libpoxicle],
  dependencies : [wayland_client, wayland_egl, egl, gles, libm])
meson.override_dependency('poxicle', poxicle_dep)
meson.override_dependency('poxicle-wl', poxicle_wl_dep)

# ---- GObject / GObject-Introspection binding (opt-in) ----------------------
# A thin GObject wrapper around the sim core so language bindings — notably the
# GNOME Shell extension (gjs) — drive the SAME C engine the KWin effect links,
# instead of maintaining a separate port. Installs a shared lib + a Poxicle-1.0
# typelib. The core engine stays dependency-free; only this target pulls GObject.
if get_option('introspection')
  gobject = dependency('gobject-2.0')
  gnome   = import('gnome')

  libpoxicle_gobject = shared_library('poxicle-gobject',
    'src/poxicle.c',
    'src/pox-gobject.c',
    include_directories : inc,
    dependencies : [gobject, libm],
    install : true)

  gnome.generate_gir(libpoxicle_gobject,
    sources           : ['src/pox-gobject.c', 'include/pox-gobject.h'],
    namespace         : 'Poxicle',
    nsversion         : '1.0',
    identifier_prefix : 'Poxicle',
    symbol_prefix     : 'poxicle',
    includes          : ['GObject-2.0'],
    header            : 'pox-gobject.h',
    install           : true)
endif

if get_option('demos')
  # Reference consumer: a standalone Wayland/EGL window that links the library.
  executable('poxicle-demo',
    'demo/main.c',
    xdg_shell_h,
    xdg_shell_c,
    include_directories : inc,
    dependencies : [wayland_client, wayland_egl, egl, gles, libm],
    link_with : libpoxicle,
    install : false)

  # Host demo: a plain opaque window with the overlay riding on a subsurface.
  executable('poxicle-host',
    'demo/host.c',
    xdg_shell_h,
    xdg_shell_c,
    include_directories : inc,
    dependencies : [wayland_client, wayland_egl, egl, gles, libm],
    link_with : [libpoxicle_wl, libpoxicle],
    install : false)
endif

# Standalone GTK4 configurator (opt-in). Toolkit-only; see configurator/meson.build.
# The KWin effect (kwin/) is a separate CMake/ECM build — KDE mandates it — and
# compiles the engine straight from ../src.
if get_option('configurator')
  subdir('configurator')
endif
