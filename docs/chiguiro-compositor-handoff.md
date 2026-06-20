# Handoff: Chiguiro renders edge particles through the poxicle-kwin compositor

**Status:** investigated, not started. Design agreed at the architecture level;
two product decisions to confirm before Phase 1 (see "Open decisions").

**Goal (user, 2026-06-18):** Chiguiro keeps configuring its own particles in-app
(the App Glass per-process presets / terminal / overscroll / colours — unchanged)
but **renders through the poxicle-kwin compositor effect** instead of GSK or the
in-app `poxicle-wl` subsurface. Chiguiro becomes a *config + sim + trigger
producer*; KWin does the drawing. This is the "FUTURE DIRECTION" noted in the
`poxicle-compositor-effects` memory.

Related docs/memories: `poxicle/docs/kwin-effect-handoff.md` (the effect itself),
memories `poxicle-compositor-effects`, `poxicle-particle-library`,
`poxicle-perf-gpu-bound`, `edge-particle-engine-perf`, `dont-kill-host-chiguiro`,
`commits-signed-by-user-only`.

---

## Why this is ~90% already built

Chiguiro already has a full **sim → instance-stream → poxicle-gl** pipeline; the
in-app overlay merely consumes it *in-process*. Moving to the compositor =
redirect that same stream across the process boundary to the effect, which
already knows how to draw such a stream.

Existing pieces (do NOT rebuild):

- **The sink** — `src/kgx-edge-draw.{h,c}`:
  `kgx_particle_sink_set_active / is_active / set_origin / set_wake /
  new_frame / clear / take`. Captures the exact `KgxParticleInstance[]` every
  frame (double-buffered, with a wake hook + active flag). Driven by Chiguiro's
  **full** sim: all presets, overscroll, ambient, **and terminal-internal
  triggers (bell / process spawn+exit / exit code)** that the compositor can
  never observe on its own. This is the whole reason we stream instances rather
  than port triggers.
- **Byte-identical records** — `src/kgx-particle.h` `KgxParticleInstance`
  (`px, py, size, angle, KgxParticleShape shape, GdkRGBA color`) is laid out
  identically to poxicle's `PoxInstance` and pinned by `G_STATIC_ASSERT`s in
  `src/kgx-poxicle.c` (lines ~30-40). ~36 bytes/instance. Zero-copy.
- **The in-app consumer** — `src/kgx-poxicle.c`: feeds the sink to `pox_wl` (an
  in-app `wl_subsurface` over Chiguiro's window) via `pox_wl_set_source`
  (`poxicle_source` reads `kgx_particle_sink_take`; `poxicle_wake` →
  `pox_wl_wake`). The compositor producer is a **sibling** of this file.
- **Attach/detach site** — `src/kgx-window.c:393` (`kgx_poxicle_attach`) and
  `:400` (`kgx_poxicle_detach`), on window map/unmap. A compositor-stream
  attach/detach hook goes alongside.
- **The gate** — `poxicle-overlay` GSetting (default true) + `KGX_POXICLE` env
  override select the in-app overlay today. The compositor backend becomes a new
  selectable value (see Open decision #1).
- **The effect already draws external instances** — `poxicle-kwin/src/poxicleeffect.{h,cpp}`:
  each `WinFx` holds `std::vector<PoxInstance> instances`; `prePaintScreen`
  fills it via `pox_engine_tick`; `paintWindow` draws it via `pox_gl_render_mvp`
  (offset by the `EffectWindow` rect, with the move-tracking MVP translate);
  `prePaintWindow` expands the device paint band; `postPaintScreen` does the
  idle-parked per-band repaint. **External-source mode only swaps where
  `instances` come from** — the draw path is untouched.

So the only genuinely new code is: a cross-process **channel** + a Chiguiro-side
**producer** + an effect-side **external-source mode**.

---

## Recommended approach: stream instances (Chiguiro sims, compositor draws)

Rejected alternative: port Chiguiro's triggers into the effect's sim. That throws
away the reuse above and *cannot* represent terminal-internal events. Don't.

Clean split that falls out:

- Effect **owns-sim mode** (existing): per-app rules (`poxconfig`), WM triggers
  (open/close/focus), and the focus-following **active-window overlay** (added
  2026-06-18: `m_activeWindow` / `m_activeEngine`) — for normal apps.
- Effect **external-source mode** (new): for windows whose client streams its own
  instances (Chiguiro). The client drives everything; the effect's own rules
  don't apply to those windows. (The active-window overlay can still layer on top
  of a Chiguiro window if desired — nice synergy, optional.)

The effect needs **zero** Chiguiro config. All tunables/colours/overrides are
applied by Chiguiro's sim before the instances exist.

---

## The new piece: the Chiguiro -> effect channel

The in-app subsurface got positioning + identity for free (a subsurface rides its
parent). The compositor path must solve both explicitly — this is the only hard
part.

### Transport (cheap, solvable)

Per Chiguiro toplevel:
- A `memfd` shared region:
  - header: `uint32 generation` (bumped on each new frame), `uint32 count`,
    window-local origin if needed, capacity;
  - body: `PoxInstance[capacity]` (~36 B each; a few hundred instances ⇒
    ~10-15 KB).
- D-Bus control (set up once, not per frame):
  - `Register(pid, memfd, ...)` hands the fd to the effect;
  - a **wake** signal so a parked effect restarts — reuse the existing
    `kgx_particle_sink_set_wake` hook, fire a D-Bus signal when a non-empty frame
    is promoted (mirrors `poxicle_wake` -> `pox_wl_wake`);
  - `Unregister` on window unmap.
- The effect **polls the shm `generation`** each compositor frame for its tracked
  Chiguiro windows; copies + offsets instances by the `EffectWindow` rect;
  `addRepaint` while `generation` advances; parks after N idle frames (mirror
  `pox_wl` `POX_WL_IDLE_GRACE`). No per-frame D-Bus.

Who owns the D-Bus name: simplest is the **effect** exposes an interface (KWin
effects can register a `QDBusConnection` object) e.g. `org.kde.KWin` child or a
dedicated `org.ninez.PoxicleBridge`; Chiguiro calls `Register`/`Unregister` and
emits wake. (The active-window work already showed the GTK side owning a name +
KWin scripting; here it's the reverse direction.)

### Window identity (the real risk)

A Wayland client can't get a KWin window handle; the effect can't get the client's
`wl_surface` id. A custom Wayland protocol would be cleanest but is **not
reachable from an effect plugin** (that's compositor-core territory).

Pragmatic match: **PID + geometry**.
- `EffectWindow` exposes `pid()`, `windowClass()`, `frameGeometry()`.
- Chiguiro registers each stream with its `pid` (+ writes current window size into
  the shm header). The effect binds a registered stream to the `EffectWindow`
  whose `pid()` matches; for multiple same-pid windows it disambiguates by
  size/geometry and re-checks on move/resize.
- Registration may race window creation — re-resolve unbound streams when new
  windows appear (`slotWindowAdded`) and when geometry changes.

This is the part to harden in Phase 3. Single-Chiguiro-window is a fine v1
(pid match alone) if multi-window matching is deferred (Open decision #2).

### Coordinates / HiDPI

Wayland clients cannot read their absolute screen position — so Chiguiro sends
**window-local** coords (origin 0) and the effect offsets by the `EffectWindow`
rect (exactly what it already does for its own sim). Scale: the effect works in
logical coords via KWin's projection; confirm Chiguiro emits logical-px
instances (the in-app path's HiDPI was deferred — `poxicle-particle-library`
memory). Verify on the 5 MP fractional-scale panel.

---

## Phased plan

- **Phase 1 — effect side + channel.** Add external-source mode to
  `poxicleeffect`: a `WinFx` variant whose `instances` come from a mapped shm
  instead of `pox_engine_tick`; the D-Bus `Register/Unregister/wake` interface;
  shm generation polling + idle parking. Test with a ~30-line standalone producer
  (no Chiguiro yet) that maps a memfd and animates a few instances.
- **Phase 2 — Chiguiro producer.** New sibling to `kgx-poxicle.c` (e.g.
  `kgx-poxicle-kwin.c`): a sink consumer that writes `kgx_particle_sink_take`
  output into the shm + bumps generation + registers/wakes/unregisters over
  D-Bus. Hook attach/detach from `kgx-window.c` (alongside the existing calls).
  Gate behind the renderer setting (Open decision #1) so it's A/B-able against
  GSK and the in-app subsurface.
- **Phase 3 — identity + robustness.** Multi-window matching, restack,
  move/resize, HiDPI, teardown races (the in-app path's UAF-on-close lesson:
  cancel/clear before free). Idle parking must not keep KWin awake.
- **Phase 4 — default.** Make compositor the default renderer once solid; keep
  in-app subsurface + GSK as fallbacks.

---

## Open decisions (confirm before Phase 1)

1. **Replace vs. third backend?** Recommended: Chiguiro gains a *selectable*
   renderer — GSK / in-app subsurface / **compositor** — so the compositor path
   is A/B-testable and the in-app overlay stays a fallback; default to compositor
   once proven. (Alternative: clean replacement of the in-app subsurface.)
2. **Identity scope for v1.** Recommended: start with PID+geometry but accept
   single-Chiguiro-window correctness for v1 and defer robust multi-window
   matching to Phase 3. Confirm that's acceptable.

---

## Operational gotchas (carried from `poxicle-compositor-effects`)

- **Effect code changes need a real KWin restart** = log out / log back in on
  Wayland (no `kwin_wayland --replace`). The D-Bus `unloadEffect`+`loadEffect`
  trick does NOT reload a recompiled `.so` (QPluginLoader keeps the old lib
  mapped). Iterate with `poxicle-kwin/run-nested.sh` (fresh nested kwin_wayland,
  no host relogin).
- **Install with `poxicle-kwin/install.sh`**, not bare `cmake --install` — needs
  `-DKDE_INSTALL_PLUGINDIR=lib/qt6/plugins` or the `.so` lands where KWin won't
  scan it.
- **Don't kill the host Chiguiro** Claude runs in (`dont-kill-host-chiguiro`);
  test with the devel build.
- `grim` can't screenshot on KWin (no wlr-screencopy) — rely on the user /
  Spectacle for visual confirmation.
- Commits: signed + authored by the user, no AI trailers
  (`commits-signed-by-user-only`). Chiguiro repo **bans AI-generated upstream
  contributions** — keep generated code local; this doc lives in the poxicle repo
  on purpose.

## Key file pointers

Chiguiro: `src/kgx-edge-draw.{h,c}` (sink), `src/kgx-particle.h` (instance/tunables),
`src/kgx-poxicle.c` (in-app consumer to mirror), `src/kgx-window.c:393/400`
(attach site), `data/org.ninez.Chiguiro.gschema.xml.in` (`poxicle-overlay` +
the new renderer setting).
poxicle-kwin: `src/poxicleeffect.{h,cpp}` (WinFx, prePaintScreen/paintWindow/
prePaintWindow/postPaintScreen, the active-window overlay), `src/poxconfig.*`.
poxicle: `include/poxicle.h` (`PoxInstance`), `include/poxicle-gl.h`
(`pox_gl_render_mvp`), `include/poxicle-wl.h` (`pox_wl_set_source` — the
in-process seam being generalised cross-process).
