/* poxicle demo — standalone transparent window.
 *
 * Owns the Wayland surface, EGL context, and frame loop; delegates simulation to
 * poxicle-core (pox_engine) and drawing to poxicle-gl (pox_gl). This is the
 * reference consumer of the library — no rendering code of its own.
 */
#define _POSIX_C_SOURCE 200809L

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include "xdg-shell-client-protocol.h"
#include "poxicle.h"
#include "poxicle-gl.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Wayland ---- */
static struct wl_display    *display;
static struct wl_compositor *compositor;
static struct xdg_wm_base   *wm_base;
static struct wl_surface    *surface;
static struct xdg_surface   *xsurface;
static struct xdg_toplevel  *toplevel;

/* ---- EGL ---- */
static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static EGLConfig  egl_cfg;
static EGLSurface egl_surf = EGL_NO_SURFACE;
static struct wl_egl_window *egl_win;

/* ---- state ---- */
static int  win_w = 900, win_h = 600;
static bool configured = false;
static bool running = true;
static int64_t last_ms;

/* ---- poxicle ---- */
#define MAX_INST 4096
static PoxInstance pbuf[MAX_INST];
static PoxEngine  *engine;
static PoxGL      *renderer;

static int64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void render(void)
{
  int64_t t = now_ms();
  double dt = (t - last_ms) / 1000.0;
  last_ms = t;
  if (dt > 0.1) dt = 0.1;            /* clamp after a stall */

  size_t n = pox_engine_tick(engine, dt, pbuf, MAX_INST);

  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  pox_gl_render(renderer, pbuf, n, win_w, win_h);
}

/* ---- frame callback loop ---- */
static const struct wl_callback_listener frame_listener;

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
  (void) data; (void) time;
  wl_callback_destroy(cb);
  render();
  struct wl_callback *next = wl_surface_frame(surface);
  wl_callback_add_listener(next, &frame_listener, NULL);
  eglSwapBuffers(egl_dpy, egl_surf);
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

/* ---- xdg / registry plumbing ---- */
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
  if (w > 0 && h > 0) {
    win_w = w; win_h = h;
    if (egl_win) wl_egl_window_resize(egl_win, w, h, 0, 0);
    if (engine) pox_engine_set_surface(engine, w, h, 1);
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

static void egl_init(void)
{
  egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
  if (egl_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no EGL display\n"); exit(1); }
  eglInitialize(egl_dpy, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  const EGLint cfg_attr[] = {
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_NONE,
  };
  EGLint n = 0;
  if (!eglChooseConfig(egl_dpy, cfg_attr, &egl_cfg, 1, &n) || n == 0) {
    fprintf(stderr, "no EGL config\n"); exit(1);
  }
  const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
  egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, ctx_attr);

  egl_win  = wl_egl_window_create(surface, win_w, win_h);
  egl_surf = eglCreateWindowSurface(egl_dpy, egl_cfg, (EGLNativeWindowType) egl_win, NULL);
  eglMakeCurrent(egl_dpy, egl_surf, egl_surf, egl_ctx);
  eglSwapInterval(egl_dpy, 1);
}

int main(void)
{
  display = wl_display_connect(NULL);
  if (!display) { fprintf(stderr, "no Wayland display\n"); return 1; }

  struct wl_registry *reg = wl_display_get_registry(display);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  wl_display_roundtrip(display);
  if (!compositor || !wm_base) { fprintf(stderr, "missing globals\n"); return 1; }

  surface  = wl_compositor_create_surface(compositor);
  xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
  xdg_surface_add_listener(xsurface, &xsurf_listener, NULL);
  toplevel = xdg_surface_get_toplevel(xsurface);
  xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
  xdg_toplevel_set_title(toplevel, "poxicle demo");
  xdg_toplevel_set_app_id(toplevel, "org.poxicle.Demo");
  wl_surface_commit(surface);

  while (!configured)
    wl_display_dispatch(display);

  egl_init();

  renderer = pox_gl_new();
  if (!renderer) { fprintf(stderr, "renderer init failed\n"); return 1; }

  engine = pox_engine_new();
  PoxTunables tn;
  pox_tunables_default(&tn);
  pox_engine_set_tunables(engine, &tn);
  pox_engine_set_surface(engine, win_w, win_h, 1);
  pox_engine_set_ambient(engine, 1);

  last_ms = now_ms();
  render();
  struct wl_callback *cb = wl_surface_frame(surface);
  wl_callback_add_listener(cb, &frame_listener, NULL);
  eglSwapBuffers(egl_dpy, egl_surf);

  while (running && wl_display_dispatch(display) != -1)
    ;

  pox_gl_free(renderer);
  pox_engine_free(engine);
  return 0;
}
