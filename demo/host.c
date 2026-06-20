/* poxicle host demo — exercises the wl subsurface backend.
 *
 * A plain opaque window (solid colour via wl_shm — stands in for "some app's
 * content") with a poxicle particle overlay riding on a subsurface above it.
 * Proves the embedding path: the overlay is transparent + click-through and
 * animates independently of the host's (here static) content.
 */
#define _GNU_SOURCE

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "poxicle.h"
#include "poxicle-wl.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static struct wl_display       *display;
static struct wl_compositor    *compositor;
static struct wl_subcompositor  *subcompositor;
static struct wl_shm           *shm;
static struct xdg_wm_base      *wm_base;
static struct wl_surface       *surface;
static struct xdg_surface      *xsurface;
static struct xdg_toplevel     *toplevel;

static PoxWl *overlay;
static int  win_w = 900, win_h = 600;
static bool configured = false;
static bool running = true;

/* A solid-colour wl_shm buffer the size of the window (XRGB8888). */
static struct wl_buffer *make_buffer(int w, int h)
{
  int stride = w * 4;
  int size = stride * h;
  int fd = memfd_create("poxicle-host", MFD_CLOEXEC);
  if (fd < 0) return NULL;
  if (ftruncate(fd, size) < 0) { close(fd); return NULL; }

  uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) { close(fd); return NULL; }
  for (int i = 0; i < w * h; i++)
    px[i] = 0x001e1e28;                 /* opaque dark blue-grey (X ignored) */
  munmap(px, size);

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                    WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return buf;
}

static void paint_parent(void)
{
  struct wl_buffer *buf = make_buffer(win_w, win_h);
  if (!buf) return;
  struct wl_region *opaque = wl_compositor_create_region(compositor);
  wl_region_add(opaque, 0, 0, win_w, win_h);
  wl_surface_set_opaque_region(surface, opaque);
  wl_region_destroy(opaque);
  wl_surface_attach(surface, buf, 0, 0);
  wl_surface_damage_buffer(surface, 0, 0, win_w, win_h);
  wl_surface_commit(surface);
}

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void) d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{ (void) d; xdg_surface_ack_configure(s, serial); configured = true; }
static const struct xdg_surface_listener xsurf_listener = { .configure = xsurf_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                          struct wl_array *states)
{
  (void) d; (void) t; (void) states;
  if (w > 0 && h > 0 && (w != win_w || h != win_h)) {
    win_w = w; win_h = h;
    if (overlay) {
      paint_parent();
      pox_wl_resize(overlay, w, h, 1);
    }
  }
}
static void top_close(void *d, struct xdg_toplevel *t)
{ (void) d; (void) t; running = false; }
static const struct xdg_toplevel_listener top_listener = {
  .configure = top_configure, .close = top_close,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
  (void) d; (void) ver;
  if (!strcmp(iface, wl_compositor_interface.name))
    compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  else if (!strcmp(iface, wl_subcompositor_interface.name))
    subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
  else if (!strcmp(iface, wl_shm_interface.name))
    shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  else if (!strcmp(iface, xdg_wm_base_interface.name)) {
    wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
  }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void) d; (void) r; (void) name; }
static const struct wl_registry_listener reg_listener = {
  .global = reg_global, .global_remove = reg_remove,
};

int main(void)
{
  display = wl_display_connect(NULL);
  if (!display) { fprintf(stderr, "no Wayland display\n"); return 1; }

  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  wl_display_roundtrip(display);
  if (!compositor || !subcompositor || !shm || !wm_base) {
    fprintf(stderr, "missing globals\n"); return 1;
  }

  surface  = wl_compositor_create_surface(compositor);
  xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
  xdg_surface_add_listener(xsurface, &xsurf_listener, NULL);
  toplevel = xdg_surface_get_toplevel(xsurface);
  xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
  xdg_toplevel_set_title(toplevel, "poxicle host");
  xdg_toplevel_set_app_id(toplevel, "org.poxicle.Host");
  wl_surface_commit(surface);

  while (!configured)
    wl_display_dispatch(display);

  paint_parent();    /* give the parent content */

  overlay = pox_wl_new(display, compositor, subcompositor, surface, win_w, win_h, 1);
  if (!overlay) { fprintf(stderr, "overlay init failed\n"); return 1; }
  pox_engine_set_ambient(pox_wl_engine(overlay), 1);
  wl_surface_commit(surface);   /* map the subsurface */

  while (running && wl_display_dispatch(display) != -1)
    ;

  pox_wl_free(overlay);
  return 0;
}
