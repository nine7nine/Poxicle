/* poxicle-wl.c — Wayland subsurface host backend (see poxicle-wl.h).
 *
 * Owns a transparent, click-through, desynchronized wl_subsurface over the
 * host's window, with its own EGL/GLES3 context. Self-animates: each subsurface
 * frame callback ticks the sim and draws via poxicle-gl, then commits the
 * subsurface independently of the host's own drawing.
 */
#define _POSIX_C_SOURCE 200809L

#include "poxicle-wl.h"
#include "poxicle-gl.h"

#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define POX_WL_MAX_INST 4096

struct PoxWl {
  struct wl_surface    *surface;     /* the subsurface's wl_surface */
  struct wl_subsurface *subsurface;
  struct wl_egl_window *egl_win;

  EGLDisplay egl_dpy;
  EGLContext egl_ctx;
  EGLConfig  egl_cfg;
  EGLSurface egl_surf;

  PoxEngine *engine;
  PoxGL     *renderer;
  PoxInstance buf[POX_WL_MAX_INST];

  PoxWlSource source;
  void       *source_user;

  struct wl_callback *frame_cb;   /* in-flight frame callback, or NULL */
  int     idle_frames;            /* consecutive frames with nothing to draw */
  int     parked;                 /* loop stopped; wake with pox_wl_wake()   */

  int     w, h, scale;
  int64_t last_ms;
};

/* Frames to keep drawing after the sim goes quiet, so the cleared (empty)
 * overlay is actually presented before we stop committing. */
#define POX_WL_IDLE_GRACE 2

static int64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const struct wl_callback_listener frame_listener;

static void draw(PoxWl *w)
{
  eglMakeCurrent(w->egl_dpy, w->egl_surf, w->egl_surf, w->egl_ctx);

  int64_t t = now_ms();
  double dt = (t - w->last_ms) / 1000.0;
  w->last_ms = t;
  if (dt > 0.1) dt = 0.1;

  size_t n = w->source
           ? w->source(w->buf, POX_WL_MAX_INST, w->source_user)
           : pox_engine_tick(w->engine, dt, w->buf, POX_WL_MAX_INST);

  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  pox_gl_render(w->renderer, w->buf, n, w->w, w->h);

  /* Idle parking: a desync subsurface that keeps committing every vblank keeps
   * the GPU and compositor awake forever. Once nothing is animating, draw a few
   * trailing frames to flush the now-empty overlay, then stop scheduling frame
   * callbacks. pox_wl_wake() restarts the loop when work arrives. In source mode
   * "active" is "the source produced particles"; otherwise it is the engine. */
  int active = w->source ? (n > 0) : pox_engine_active(w->engine);
  w->idle_frames = active ? 0 : w->idle_frames + 1;

  if (w->idle_frames <= POX_WL_IDLE_GRACE) {
    /* schedule the next frame on our own surface, then commit via swap */
    w->frame_cb = wl_surface_frame(w->surface);
    wl_callback_add_listener(w->frame_cb, &frame_listener, w);
  } else {
    w->frame_cb = NULL;
    w->parked = 1;
  }
  eglSwapBuffers(w->egl_dpy, w->egl_surf);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
  PoxWl *w = data;
  (void) time;
  wl_callback_destroy(cb);
  if (w->frame_cb == cb)
    w->frame_cb = NULL;
  draw(w);
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static int egl_setup(PoxWl *w, struct wl_display *display)
{
  w->egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
  if (w->egl_dpy == EGL_NO_DISPLAY) return 0;
  eglInitialize(w->egl_dpy, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  const EGLint cfg_attr[] = {
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_NONE,
  };
  EGLint n = 0;
  if (!eglChooseConfig(w->egl_dpy, cfg_attr, &w->egl_cfg, 1, &n) || n == 0)
    return 0;

  const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
  w->egl_ctx = eglCreateContext(w->egl_dpy, w->egl_cfg, EGL_NO_CONTEXT, ctx_attr);
  if (w->egl_ctx == EGL_NO_CONTEXT) return 0;

  w->egl_win  = wl_egl_window_create(w->surface, w->w, w->h);
  w->egl_surf = eglCreateWindowSurface(w->egl_dpy, w->egl_cfg,
                                       (EGLNativeWindowType) w->egl_win, NULL);
  if (w->egl_surf == EGL_NO_SURFACE) return 0;

  eglMakeCurrent(w->egl_dpy, w->egl_surf, w->egl_surf, w->egl_ctx);
  eglSwapInterval(w->egl_dpy, 1);
  return 1;
}

PoxWl *pox_wl_new(struct wl_display *display,
                  struct wl_compositor *compositor,
                  struct wl_subcompositor *subcompositor,
                  struct wl_surface *parent,
                  int width, int height, int scale)
{
  if (!display || !compositor || !subcompositor || !parent) return NULL;

  PoxWl *w = calloc(1, sizeof *w);
  if (!w) return NULL;
  w->w = width > 0 ? width : 1;
  w->h = height > 0 ? height : 1;
  w->scale = scale > 0 ? scale : 1;

  w->surface = wl_compositor_create_surface(compositor);
  w->subsurface = wl_subcompositor_get_subsurface(subcompositor, w->surface, parent);
  wl_subsurface_set_desync(w->subsurface);
  wl_subsurface_set_position(w->subsurface, 0, 0);

  /* Click-through: an empty input region makes events fall to the parent. */
  struct wl_region *empty = wl_compositor_create_region(compositor);
  wl_surface_set_input_region(w->surface, empty);
  wl_region_destroy(empty);

  if (!egl_setup(w, display)) { pox_wl_free(w); return NULL; }

  w->renderer = pox_gl_new();
  if (!w->renderer) { pox_wl_free(w); return NULL; }

  w->engine = pox_engine_new();
  pox_engine_set_surface(w->engine, w->w, w->h, w->scale);

  w->last_ms = now_ms();
  draw(w);   /* first frame + schedules the animation loop */
  return w;
}

void pox_wl_free(PoxWl *w)
{
  if (!w) return;
  /* Cancel any in-flight frame callback before tearing down its surface, or it
   * fires after the free and dispatches draw() on freed memory (use-after-free
   * in wl_surface_frame). */
  if (w->frame_cb) { wl_callback_destroy(w->frame_cb); w->frame_cb = NULL; }
  if (w->engine) pox_engine_free(w->engine);
  if (w->renderer) {
    eglMakeCurrent(w->egl_dpy, w->egl_surf, w->egl_surf, w->egl_ctx);
    pox_gl_free(w->renderer);
  }
  if (w->egl_surf != EGL_NO_SURFACE) eglDestroySurface(w->egl_dpy, w->egl_surf);
  if (w->egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(w->egl_dpy, w->egl_ctx);
  if (w->egl_win) wl_egl_window_destroy(w->egl_win);
  if (w->subsurface) wl_subsurface_destroy(w->subsurface);
  if (w->surface) wl_surface_destroy(w->surface);
  free(w);
}

void pox_wl_resize(PoxWl *w, int width, int height, int scale)
{
  if (!w || width <= 0 || height <= 0) return;
  w->w = width; w->h = height; w->scale = scale > 0 ? scale : 1;
  if (w->egl_win) wl_egl_window_resize(w->egl_win, width, height, 0, 0);
  pox_engine_set_surface(w->engine, width, height, w->scale);
}

void pox_wl_set_position(PoxWl *w, int x, int y)
{
  if (!w || !w->subsurface) return;
  wl_subsurface_set_position(w->subsurface, x, y);
}

void pox_wl_set_source(PoxWl *w, PoxWlSource source, void *user)
{
  if (!w) return;
  w->source = source;
  w->source_user = user;
}

void pox_wl_wake(PoxWl *w)
{
  if (!w || !w->parked) return;
  w->parked = 0;
  w->idle_frames = 0;
  w->last_ms = now_ms();   /* drop the parked gap so dt doesn't jump */
  draw(w);                 /* restarts the frame-callback loop */
}

PoxEngine *pox_wl_engine(PoxWl *w) { return w ? w->engine : NULL; }
