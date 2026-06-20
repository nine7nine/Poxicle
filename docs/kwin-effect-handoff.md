# poxicle → KWin compositor effect — research, plan & implementation notes

Handoff document. Goal: a **KWin (Plasma 6, Wayland-only) binary OpenGL effect** that draws
poxicle edge particles **per-window / per-window-state**, reusing poxicle's simulation core,
tunables, and trigger model. poxicle keeps its in-app library support (`poxicle-wl` / byoc)
unchanged. The architecture stays portable for a later GNOME / other-compositor renderer.

**Status (scaffold landed):** the poxicle MVP renderer change (§3) is implemented and
builds (normal + ASAN). A sibling project `poxicle-kwin/` (M0/M1) **compiles and links into a
valid `poxicle_kwin.so`** against the installed KWin 6.6.5 — per-window `PoxEngine` map, lifecycle
wiring, tick loop, repaint scheduling, the `pox_gl_render_mvp` draw with GL state save/restore, and
one trigger demo. Not yet done: visual verification in a nested KWin, KConfig wiring (M3), damage
regions / multi-output / GLES-vs-GL dialect (M4). Build notes learned the hard way are in §8.

**Scope locked in:**
- Target **KWin 6.6.5 and newer only**. No back-compat with older Plasma.
- **Wayland only.** No X11 paths, no `kwin-x11` target.
- KDE/KWin is the *first* consumer; GNOME/Mutter or others may follow — keep the seam clean (§13).
- All findings below are grounded in the installed headers at `/usr/include/kwin/` (KWin 6.6.5),
  a real third-party effect (`kwin-effects-forceblur`), and the KDE wikis (§14).

---

## 1. Why this is a clean fit

poxicle is already a three-layer split:

```
sim core    poxicle.c / poxicle.h     pure C, emits PoxInstance[]   ← REUSE VERBATIM
renderer    poxicle-gl.c              needs only a *current* GL ctx  ← reuse w/ tiny change (§3)
host backend poxicle-wl.c             Wayland subsurface + EGL       ← DROP for KWin
```

A KWin effect is just **a new host backend that replaces `poxicle-wl` entirely**. KWin *is* the
compositor: it hands you a current GL context inside its paint pass — that is exactly the
"bring-your-own-context" path `poxicle-gl.h` already anticipates ("works under the wl subsurface
host, a bring-your-own-context host, **or (later) a compositor effect**").

Dropping `poxicle-wl` for this consumer also drops all the subsurface/EGL/frame-callback
machinery **and the Mesa-EGL leak surface** we saw under ASAN.

The reusable seam is `PoxInstance[]` + the sim engine + `PoxTunables`. Everything KWin-specific
lives in the new effect repo, never in poxicle (same discipline that kept Chiguiro wiring out of
the library).

**Per-window / per-app is KWin's home turf, not a limitation.** A single app (Chiguiro) can only
decorate its own window. KWin knows *every* window's app-id, geometry, and state globally and
emits signals for all of it — so "particles on app X only", "ambient on the active window",
"burst when a window demands attention" all map directly, and are richer than anything an
in-app overlay can do.

---

## 2. KWin 6.6.5 API facts (from installed headers)

### Effect base class — `/usr/include/kwin/effect/effect.h`
Subclass `KWin::Effect`. Key virtuals (note the 6.x signatures use `RenderTarget` / `RenderViewport`):

```cpp
void prePaintScreen(ScreenPrePaintData &data, std::chrono::milliseconds presentTime) override;
void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                 int mask, const Region &deviceRegion, LogicalOutput *screen) override;
void postPaintScreen() override;
bool isActive() const override;            // exclude effect from chain when nothing animates
void reconfigure(ReconfigureFlags) override;
```

- **Draw on top of windows in `paintScreen`** — after calling `effects->paintScreen(...)`.
- **GL context is guaranteed current** during construct / destruct / `reconfigure` / all paint
  stages. Outside those (e.g. a signal slot doing GL work) call
  `effects->makeOpenGLContextCurrent()`.
- `presentTime` gives the expected display time of the frame → use it to derive `dt` for the sim.
- `isActive()` returning false lets KWin skip the effect entirely. It does **not** schedule
  frames — repaints still have to be requested (§5).

### Plugin registration & build macro
`effect.h` documents the canonical recipe:

```cpp
KWIN_EFFECT_FACTORY(PoxicleEffect, "metadata.json")
// or gate support:
// KWIN_EFFECT_FACTORY_SUPPORTED(PoxicleEffect, "metadata.json",
//     return effects->isOpenGLCompositing() && effects->waylandDisplay();)
```

```cmake
kcoreaddons_add_plugin(poxicle_kwin SOURCES ... INSTALL_NAMESPACE "kwin/effects/plugins")
```

⚠️ **Binary-compat warning (important for the handoff):** the factory IID embeds the effect API
version (currently `KWIN_EFFECT_API_VERSION 0.237`). The header states plainly: *"This API is not
providing binary compatibility and thus the effect plugin must be compiled against the same
kwineffects library version as KWin."* → **The plugin must be rebuilt whenever KWin bumps the
effect API.** Document this for users; ship as source / a rebuild script / AUR-style package.

### `effects` singleton — `effecthandler.h`
Signals (connect in the effect ctor):
`windowAdded`, `windowClosed`, `windowDeleted`, `windowActivated`, `minimizedChanged(w)`,
`windowFrameGeometryChanged(w, oldGeom)`, `windowFullScreenChanged(w)`,
`windowMaximizedStateChanged(w, h, v)`, `windowStart/Step/FinishUserMovedResized(w, ...)`,
`windowDamaged(w)`, `windowKeepAboveChanged`, `windowDesktopsChanged`, `windowHiddenChanged`,
`stackingOrderChanged`.

Methods: `activeWindow()`, `stackingOrder()`, `addRepaintFull()`, `addRepaint(region/rect/...)`,
`isOpenGLCompositing()`, `makeOpenGLContextCurrent()`, `waylandDisplay()`.

### Per-window API — `effectwindow.h`
- `QRectF frameGeometry()` — visible window rect in **logical** coords (the equivalent of the
  geometry `kgx-poxicle.c` computes via `gtk_native_get_surface_transform`).
- `QRectF expandedGeometry()` — includes shadow/CSD margins (use frameGeometry to hug the visible
  window, as Chiguiro does).
- `QString windowClass()` — `resourceName + " " + resourceClass`, i.e. the **app-id for per-app rules**.
- `QString caption()`, `isFullScreen()`, `isMinimized()`, `isNormalWindow()`, `isDesktop()`,
  `isDock()`, `isPopupWindow()`, `isDialog()`, `isOnCurrentDesktop()`, `windowType()`.

### Coordinate / projection model (decisive — see §3)
- KWin paints in **device** coordinates.
- `RenderViewport::projectionMatrix() -> QMatrix4x4` maps **logical → clip space**, already
  accounting for output scale, fractional scaling, and output transform (rotation).
- `RenderViewport::scale() -> double` is the device scale; `mapToRenderTarget(logical)` → device.
- `RenderTarget::size()` is the device-pixel size of the target framebuffer.
- KWin logical coords are y-down (top-left origin) — **same convention as poxicle's surface
  pixels** — and `projectionMatrix()` handles the y-flip into clip space, so **no manual Y flip is
  needed** if you feed logical coords through the projection matrix.

### GL utilities — `opengl/glvertexbuffer.h`, `glshadermanager.h`, `glshader.h`
- `GLVertexBuffer`: `setAttribLayout(span<GLVertexAttrib>, stride)`, `setData(ptr, bytes)`,
  `setVertexCount(n)`, `render(GLenum mode)` / `draw(mode, first, count)`.
  **No instancing / no `glVertexAttribDivisor` API.** This shapes Strategy B below.
- `ShaderManager`: `pushShader(traits|shader)`, `popShader()`,
  `loadShaderFromCode(vsBytes, fsBytes)`, `generateCustomShader(traits, vs, fs)`.
  `ShaderBinder` is an RAII push/pop helper.
- `GLShader::setUniform(name, QMatrix4x4 / QVector* / float / int / QColor)`.

---

## 3. The renderer decision (the crux)

poxicle's renderer is the only host-specific layer. Two viable strategies — **recommend A for
first-light, keep B as the hardening fallback.** The sim core, tunables, and triggers are
identical either way; only the draw swaps.

### Strategy A (recommended start): reuse `poxicle-gl`, drive it with KWin's projection

`poxicle-gl` already does SDF shapes + analytic AA + a single instanced draw well. The only thing
that makes it host-specific is its vertex shader: it computes clip space from a `u_viewport`
uniform assuming a full-framebuffer, top-left, y-down target. Under KWin that's wrong (output
offset, scale, transform).

**Small poxicle change (also benefits byoc):** add an MVP-driven entry point.
- In `poxicle-gl.c`, add a `u_mvp` (mat4) uniform and compute
  `gl_Position = u_mvp * vec4(world, 0.0, 1.0)` where `world` stays in poxicle's pixel space.
- Public API: `void pox_gl_render_mvp(PoxGL *r, const PoxInstance *insts, size_t n, const float mvp[16]);`
- Keep `pox_gl_render(...)` as a thin wrapper that builds an ortho MVP from `width/height` and
  calls the new path — so wl/byoc/demo behaviour is unchanged.
- **The MVP variant must NOT call `glViewport`** (KWin already set the output viewport). The
  current `pox_gl_render` calls `glViewport(0,0,w,h)`; gate that out of the MVP path.

**Effect side:**
- Pass `viewport.projectionMatrix().constData()` (QMatrix4x4 is column-major float[16], exactly
  what `glUniformMatrix4fv(..., GL_FALSE, ...)` wants).
- Position particles in **logical** coordinates: after ticking each window's engine (which emits
  surface-local 0..w / 0..h), **translate instances by the window's `frameGeometry().topLeft()`**
  so they become logical screen coords. The projection matrix maps them to clip, applying scale
  and transform. No manual Y flip.

**GL state hygiene (mandatory).** `poxicle-gl` binds its own program/VAO/VBO, enables `GL_BLEND`,
sets blend func, and does not restore. KWin (and `ShaderManager`) assume they own GL state. Wrap
the draw and save/restore:
- `GL_CURRENT_PROGRAM`, `GL_VERTEX_ARRAY_BINDING`, `GL_ARRAY_BUFFER_BINDING`,
- `GL_BLEND` enable + blend func (`GL_BLEND_SRC_*`/`GL_BLEND_DST_*`) + blend equation.
- Do **not** touch `glViewport` in the MVP path (see above). Restore everything after the draw.
- Failing to restore is the classic "the effect corrupts the rest of the screen" footgun.

**GLSL dialect.** poxicle shaders are `#version 300 es`. KWin on Wayland/Mesa (the user's
Intel Iris setup) uses **GLES**, so they compile as-is. KWin *can* use desktop GL on other
setups, where `300 es` fails. Mitigation: query `KWin::GLPlatform::instance()->isGLES()` and pick
`#version 300 es` vs `#version 330 core` (the SDF/AA body is otherwise identical; only the version
+ `precision` lines differ). Low risk on the target machine; document as caveat #1.

*Pros:* one renderer (poxicle-gl) serves wl + byoc + KWin; minimal new code; instancing retained.
*Cons:* raw GL inside KWin requires disciplined state save/restore.

### Strategy B (fallback): native KWin renderer via `GLVertexBuffer` + `ShaderManager`

If raw-GL state bleed or GLES/GL mismatch becomes painful, write a small C++ renderer over the
same `PoxInstance[]`:
- CPU-expand each particle to 2 triangles (6 verts) carrying corner/color/shape/rotation.
- Upload via `GLVertexBuffer::streamingBuffer()` with `setAttribLayout`.
- Draw with a custom shader from `ShaderManager::loadShaderFromCode(vs, fs)` — the SDF fragment
  shader ports verbatim; the vertex shader uses `viewport.projectionMatrix()` as the MVP uniform.
- No instancing (negligible at our particle counts — a few thousand verts/frame on KWin's stream
  buffer is nothing).

*Pros:* fully idiomatic; KWin manages all GL state, scale, color management; ShaderManager handles
the GLSL preamble/version, so the dialect problem disappears. *Cons:* a second renderer to
maintain; loses the single-instanced-draw elegance.

**Recommendation:** ship M1/M2 on Strategy A (fast, max reuse). If it fights the GL state machine,
move the renderer to B — a localized change, since the seam (`PoxInstance[]`) doesn't move.

---

## 4. Per-window / per-app model

Maintain per-window state keyed by `EffectWindow*`:

```cpp
struct WinFx {
    PoxEngine *engine;       // pox_engine_new()
    QRectF     geom;         // last frameGeometry (logical)
    QString    appId;        // windowClass()
    bool       active;
};
std::unordered_map<KWin::EffectWindow*, WinFx> m_windows;
```

Lifecycle wiring (connect `effects` signals in ctor):
- `windowAdded(w)` → if it matches rules (§7), create engine, `pox_engine_set_surface(w,h,scale)`,
  `pox_engine_set_tunables(...)`, optionally fire an "appear" burst.
- `windowDeleted(w)` → free engine (`pox_engine_free`) and erase. Use `EffectWindowDeletedRef` if
  you want particles to persist through the close animation; free on `windowDeleted`, **not**
  `windowClosed` (close starts the teardown; deleted means gone) — avoids use-after-free races.
- `windowFrameGeometryChanged(w, old)` / `windowStepUserMovedResized` → update geom +
  `pox_engine_set_surface`. (Resize tracks every frame during interactive resize.)
- `windowActivated(w)` → toggle ambient / fire a burst per config.
- `minimizedChanged`, `windowFullScreenChanged` → start/stop/restyle.

**Filtering ("per-app / per-state"):** match `windowClass()` against a configured allow/deny list;
gate on `isNormalWindow()` (skip docks/desktop/popups/menus); per-state rules (active-only,
fullscreen behaviour). This is the surface that "recycles how Chiguiro configures poxicle" — same
tunables/semantics, sourced from KConfig instead of GSettings (§7).

**Geometry → poxicle surface.** Each engine frames a surface of `(w,h)` and emits instances in
surface-local pixels. After tick, translate by `frameGeometry().topLeft()`; concatenate all
windows' instances into one buffer and issue a single `pox_gl_render_mvp` per frame (fewer draws),
or draw per-window. Use `frameGeometry` (visible window) — the same choice `kgx-poxicle.c` makes —
not `expandedGeometry`, unless you deliberately want particles out in the shadow margin.

**Scale.** Keep positions logical and let `projectionMatrix()` apply device scale. Feed poxicle a
scale hint (`round(viewport.scale())`) so particle density/sizing matches HiDPI, but **don't
double-apply** scale to positions. Verify on the 5 MP panel during M2 — this is a known fiddly bit.

---

## 5. Frame loop / repaint scheduling

KWin only repaints damage; an animating overlay must request frames.

- In `prePaintScreen(data, presentTime)`: compute `dt` from `presentTime` (store the previous
  value); for each live engine call `pox_engine_tick(dt, buf, cap)`; accumulate instances. If any
  engine is active, extend the paint region (`data.paint += <edge bands>`).
- In `paintScreen(...)`: after `effects->paintScreen(...)`, draw the accumulated instances with
  the renderer (§3), using `viewport.projectionMatrix()`.
- In `postPaintScreen()`: while any engine is active, request the next frame —
  `effects->addRepaint(<edge-band region>)` (or `effects->addRepaintFull()` for first-light
  correctness). When all engines are idle (`pox_engine_active(...)` all false), stop requesting →
  KWin sleeps. This is poxicle's "park when idle" mapped onto KWin's damage model.
- `isActive()` returns `anyEngineActive` so KWin drops the effect from the chain when idle.

**Damage regions:** union of each window's edge band (`frameGeometry` inflated by the max particle
reach). Start with `addRepaintFull` for correctness, then narrow to per-window bands — important
on the 5 MP panel, where this engine is overdraw/fill-bound (see the perf memory: the lever is GPU
overdraw, so keep edge bands tight).

---

## 6. Triggers (generic API → KWin events)

poxicle triggers (host fires them; they live in the consumer, never in the library):
`pox_engine_burst(pos, color)`, `pox_engine_fire_overscroll(edge, color)`,
`pox_engine_set_ambient(on)`.

Suggested KWin event → trigger mapping (all configurable):
- window activated → burst / enable ambient on it.
- window added (appear) → perimeter burst sweep.
- maximize / fullscreen toggled → edge beam (`fire_overscroll`).
- window unresponsive (`windowUnresponsiveChanged`) / demands attention → pulse or repeated burst.
  (Demands-attention may need the NET state via `windowType`/window data rather than a dedicated
  signal — verify against `effecthandler.h` window-data signals during M3.)

---

## 7. Configuration (recycle Chiguiro's tunables, source from KConfig)

- **Reuse `PoxTunables` verbatim** as the per-stream config; defaults from `pox_tunables_default`.
  The values/presets map 1:1 to what Chiguiro exposes — only the *source* changes (GSettings →
  KConfig). This is the literal "recycle how Chiguiro configures poxicle" ask.
- KWin per-effect config: `<name>.kcfg` + `.kcfgc` → generated KConfigXT class, wired with
  `kconfig_add_kcfg_files`. The config lives in `kwinrc` under group **`Effect-<id>`** (the kcfg
  has exactly one group; KWin reads it via `EffectsHandler::effectConfig(...)` which prepends
  `Effect-`). `reconfigure(flags)` reloads → re-push tunables to all engines.
- Initial schema: app allow/deny list (matched against `windowClass()`), active-window-only
  toggle, ambient on/off, plus the poxicle tunables (speed, thickness, tail length, pulse
  depth/speed, shape, colors, release modes). A QML/KCM config UI is phase 2 — for first-light,
  edit `kwinrc` directly or ship a simple `config.ui` whose widgets are named `kcfg_<Option>`.

---

## 8. Build & packaging

New repo (or a `kwin/` subdir of poxicle). CMake project. **Compile poxicle's core C files
directly into the effect target** — `poxicle.c` (+ `poxicle-gl.c` for Strategy A). They need only
GLES headers; do **not** pull in `poxicle-wl` or its Wayland/EGL deps. Vendor poxicle as a git
submodule or subtree; its meson build is irrelevant here.

CMakeLists essentials (grounded in `kwin-effects-forceblur` + the `effect.h` doc):

```cmake
cmake_minimum_required(VERSION 3.16)
project(poxicle_kwin)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(KF6 REQUIRED COMPONENTS CoreAddons Config ConfigGui I18n)
find_package(KWin REQUIRED COMPONENTS kwineffects)
find_package(epoxy REQUIRED)

kcoreaddons_add_plugin(poxicle_kwin
    SOURCES poxicle_effect.cpp pox_kwin_renderer.cpp
            poxicle/src/poxicle.c poxicle/src/poxicle-gl.c
    INSTALL_NAMESPACE "kwin/effects/plugins")

target_include_directories(poxicle_kwin PRIVATE poxicle/include)
target_link_libraries(poxicle_kwin PRIVATE
    KWin::kwin KF6::CoreAddons KF6::ConfigGui epoxy::epoxy)

# kconfig_add_kcfg_files(poxicle_kwin poxicleconfig.kcfgc)   # when adding config
```

Notes:
- `forceblur` uses `add_library(<n> MODULE ...)` + `target_link_libraries(... KWin::kwin
  KF6::ConfigGui)` + install to `${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins`. The
  `kcoreaddons_add_plugin(... INSTALL_NAMESPACE "kwin/effects/plugins")` form is the official
  shorthand that does the same — use it.
- The link target is **`KWin::kwin`** (the Wayland lib). No `KWinX11::kwin` — we're Wayland-only.
- Mark the C files as C and the `.cpp` as C++; poxicle headers are already `extern "C"`-guarded.
- `metadata.json`:
  ```json
  { "KPlugin": {
      "Id": "poxicle_kwin",
      "Name": "Poxicle Particles",
      "Description": "Edge particle effects per window",
      "Category": "Appearance",
      "EnabledByDefault": false
  } }
  ```
- Install path (Wayland): `${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins/`.

### Build gotchas (hit and resolved while scaffolding `poxicle-kwin`)

These bit during the first real build against KWin 6.6.5 on this Arch box — all fixed in the
project's `CMakeLists.txt`:
- **ECM ≥ 6.22** is required by KWin 6.6.5's CMake config. A stale `extra-cmake-modules-git`
  reporting `6.16.0` fails the gate even if the code is newer — update ECM.
- **Qt5 coexists**, so `QT_DEFAULT_MAJOR_VERSION` isn't 6 by default and KWin's transitive
  `find_dependency(Qt6Qml)` trips `qt_generate_foreign_qml_types() is only available in Qt 6`.
  Fix: `set(QT_DEFAULT_MAJOR_VERSION 6)` **before** any `find_package`. Relatedly, `/usr/bin/moc`
  is **Qt5's** moc — AUTOMOC uses Qt6's correctly, but any manual moc must use `/usr/lib/qt6/moc`.
- **Qt6 components**: `KWin::kwin`'s interface needs `Core Gui Widgets DBus OpenGL Qml Quick`
  found, or you get `cannot find -lQt6::Widgets` at link.
- **`kcoreaddons_add_plugin` needs `CMAKE_LIBRARY_OUTPUT_DIRECTORY` set** (set it to
  `${CMAKE_BINARY_DIR}/bin`, which also matches the `QT_PLUGIN_PATH` dev loop in §9).
- The AUTOMOC warning *"includes the moc file … but does not contain a Q_OBJECT … macro"* is
  **benign** for the `KWIN_EFFECT_FACTORY` pattern — moc's preprocessor expands the macro and finds
  `Q_OBJECT`; the build succeeds. The embedded IID ends up as
  `org.kde.kwin.EffectPluginFactory6.6.5` — note the version suffix = the rebuild-on-upgrade coupling.

---

## 9. Dev loop (safe — do NOT iterate on the live session's KWin)

A crash in the live compositor takes the whole desktop (and this terminal). **Develop in a nested
KWin**, never the host session:

```bash
# Isolated throwaway config dir, with the effect pre-enabled:
mkdir -p /tmp/pox-kwin-test
kwriteconfig6 --file /tmp/pox-kwin-test/kwinrc --group Plugins --key poxicle_kwinEnabled true

# Nested KWin on its own D-Bus + config, loading the effect from the build dir.
# NB: there is no --windowed flag in KWin 6 — windowed mode is implied by
# --width/--height; the backend (nested Wayland) is auto-selected in a Wayland session.
QT_PLUGIN_PATH="$PWD/build/bin" XDG_CONFIG_HOME=/tmp/pox-kwin-test \
  dbus-run-session -- kwin_wayland --width 1600 --height 1000 konsole
```

The separate `XDG_CONFIG_HOME` + `dbus-run-session` keep this off your real `kwinrc` and away from
the live KWin's D-Bus, so a crash can't take down your desktop. (`xdg-desktop-portal` "No permission
store" warnings from the throwaway bus are harmless.)

- **Logs:** run nested KWin from a terminal to see stderr — `poxicle-gl` prints shader
  compile/link errors there. Or `journalctl --user -f -t kwin_wayland`.
- **Sanitizers:** KWin itself won't run under ASAN conveniently. Keep poxicle's sim + renderer
  validated through poxicle's own `build-asan` + the `poxicle-host` demo (already set up; see the
  repo README's Sanitizers section). The effect layer is thin glue; rely on nested-KWin + logs.

---

## 10. Risks & unknowns (ranked)

1. **GL state bleed (Strategy A)** corrupting KWin's rendering — mitigate with strict
   save/restore; Strategy B avoids it entirely. High attention, low residual risk.
2. **Effect-API binary break on KWin upgrades** (API v0.237 embedded in the IID) — must rebuild
   per KWin release that bumps it. Document; ship a rebuild path.
3. **GLES vs desktop-GL shader dialect** — branch `#version` on `GLPlatform::isGLES()`. Low on the
   target machine (Mesa/Intel Wayland = GLES).
4. **Coordinate/scale correctness** across multi-output + fractional scaling — use logical coords
   + `projectionMatrix()`; verify the scale hint on the 5 MP panel.
5. **Per-frame fill cost on the 5 MP panel** — this engine is overdraw-bound; keep particle
   counts/edge bands tight and use per-window damage regions, not full repaints.
6. **Window teardown races** — use `EffectWindowDeletedRef`; free engines on `windowDeleted`.

---

## 11. Milestones (suggested handoff sequence)

| # | Deliverable | Est. |
|---|-------------|------|
| M0 | Scaffolding: `Effect` subclass + factory + metadata + CMake; loads in nested KWin, logs on enable. | ~0.5d |
| M1 | Static draw: in `paintScreen`, draw a hardcoded particle ring around the active window via `pox_gl_render_mvp` + projection + GL state save/restore. Proves the GL seam. | ~1d |
| M2 | Sim + per-window: `PoxEngine` per matched window, tick from `presentTime`, ambient look, repaint scheduling + idle parking, scale correctness. | ~1–2d |
| M3 | Triggers + config: map activate/add/attention to bursts; KConfig tunables + `reconfigure`; per-app filter. | ~1–2d |
| M4 | Hardening: multi-output/scale, damage regions, teardown races, GLES/GL `#version` branch; optional QML/KCM config UI. | ~2–4d |

~2–3 days to a convincing prototype; ~1–2 weeks to polished. Almost all of it is KWin plumbing —
poxicle's byoc design already did the hard part.

---

## 12. Reuse-vs-new summary

- **Reuse as-is:** `poxicle.c` sim core, `PoxTunables` + defaults, trigger semantics, SDF shaders.
- **Small poxicle change:** add `pox_gl_render_mvp` (MVP uniform; skip `glViewport`) to
  `poxicle-gl.c` — also improves byoc. Keep `pox_gl_render` as an ortho wrapper.
- **New (the effect repo):** C++ `Effect` subclass, per-window engine map + signal wiring, the
  KWin renderer adapter (GL state save/restore, or Strategy B), repaint scheduler, KConfig glue,
  CMake + metadata.json.
- **Dropped for KWin:** all of `poxicle-wl` (subsurface/EGL/frame-callbacks) and its Mesa-EGL leak
  surface.

---

## 13. Future portability (GNOME / others)

Keep the seam at `PoxInstance[]` + the sim core. Rules:
- Nothing KWin-specific ever leaks into poxicle; the effect repo owns all KWin code.
- A future **Mutter plugin (C)** can reuse the sim core + a Cogl/GL renderer adapter exactly like
  the KWin renderer adapter.
- GNOME **Shell extensions are JavaScript** — they can't link the C core directly. Options later:
  GObject-introspection bindings over poxicle's flat C API (it's already GI-friendly — plain
  structs, flat functions), or a Mutter C plugin. Keeping the C API flat and struct-based now keeps
  that door open at zero cost.

---

## 14. References

- KWin 6.6.5 headers (authoritative for this target): `/usr/include/kwin/effect/`,
  `/usr/include/kwin/opengl/`, `/usr/include/kwin/core/rendertarget.h`, `renderviewport.h`.
- [kwin-effects-forceblur (taj-ny)](https://github.com/taj-ny/kwin-effects-forceblur) — real
  third-party binary GL effect for Plasma 6; reference for CMake/link/config layout.
- [D3SOX/kwin-forceblur](https://github.com/D3SOX/kwin-forceblur) — another fork, install docs.
- [KWin/KConfigXT Effects — KDE Community Wiki](https://community.kde.org/KWin/KConfigXT_Effects) —
  per-effect config (kcfg group naming, `Effect-<id>`).
- [KWin Effects — KDE Developer](https://develop.kde.org/docs/plasma/kwineffect/) — (QML/declarative
  effects; the C++ binary recipe is in `effect.h`'s doc block, not this page).
- [KWinception — Martin Gräßlin](https://blog.martin-graesslin.com/blog/2015/03/kwinception/) and
  [KWin/Wayland — KDE Community Wiki](https://community.kde.org/KWin/Wayland) — nested-KWin dev loop.
</content>
</invoke>
