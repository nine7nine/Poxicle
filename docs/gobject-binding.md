# poxicle — The GObject-Introspection Binding

The binding is `src/pox-gobject.c` behind `include/pox-gobject.h`: a thin GObject wrapper that exposes the C engine as the introspected `Poxicle` namespace, so a language binding — notably the GNOME Shell extension in gjs — drives the *same* engine the KWin effect links instead of maintaining a separate port.

## Table of Contents

1. [What the binding is](#1-what-the-binding-is)
2. [The PoxicleEngine object](#2-the-poxicleengine-object)
3. [Two tick outputs: instances and vertices](#3-two-tick-outputs-instances-and-vertices)
4. [GPU-ready triangulation](#4-gpu-ready-triangulation)
5. [Config-driven look, resolved in C](#5-config-driven-look-resolved-in-c)
6. [The stream receiver](#6-the-stream-receiver)
7. [Catalogue helpers](#7-catalogue-helpers)
8. [Design rules](#8-design-rules)

---

## 1. What the binding is

The core engine is dependency-free C. The binding adds exactly one layer on top — a GObject wrapper — and nothing more, so that any GObject-Introspection consumer can use the engine without a hand-written port. It is introspected as the **`Poxicle`** namespace, version `1.0`; in gjs that is:

```js
import Pox from 'gi://Poxicle';
const engine = new Pox.Engine();
```

The wrapper is deliberately thin: it owns a `PoxEngine` and forwards. Most methods are one-line passthroughs to the [`pox_engine_*`](simulation-core.gen.html) calls, with `g_return_if_fail` guards and gobject-introspection annotations. The only substantial logic the binding adds — and the reason it isn't a trivial shim — is the config resolver (§5) and the stream receiver (§6), both of which would otherwise have to be re-implemented in every binding language.

The build produces a shared library `libpoxicle-gobject` plus a `Poxicle-1.0.typelib`; see [Build, Install & Packaging](build.gen.html) for how it is generated and installed so gjs finds `gi://Poxicle`.

## 2. The PoxicleEngine object

`PoxicleEngine` is a `G_DECLARE_FINAL_TYPE` deriving from `GObject`. It wraps a `PoxEngine` created in `init` and freed in `finalize` (which also detaches any attached stream). The surface, look and clock methods map straight through:

| Binding method | Wraps |
| --- | --- |
| `poxicle_engine_set_surface(w, h, scale)` | `pox_engine_set_surface` |
| `poxicle_engine_set_corner_radius(top, bottom)` | `pox_engine_set_corner_radius` |
| `poxicle_engine_set_seed(seed)` | `pox_engine_set_seed` |
| `poxicle_engine_set_edge_mask(top, right, bottom, left)` | `pox_engine_set_edge_mask` |
| `poxicle_engine_set_preset(name, reverse)` | `pox_preset_tunables` + `pox_engine_set_kind` |
| `poxicle_engine_set_palette(palette_id)` | `pox_engine_set_palette` |

`set_preset` is the convenience the [configurator](configuration.gen.html) presets map onto: it copies a named preset's seed tunables, falls back to defaults for an unknown name, and applies the matching emission kind in one call. The corner-radius, seed, and edge-mask methods carry the same semantics as the core — `top` for the top corners (KDE), `bottom` for all four (GNOME); a per-object seed so independent rings don't lockstep; an edge mask for surfaces docked to a screen border.

## 3. Two tick outputs: instances and vertices

The binding exposes two per-frame outputs, both returning a `GBytes` blob a binding can read without copying structs by hand.

- **`poxicle_engine_tick(dt)`** returns the raw `PoxInstance` array — 36 bytes per instance, little-endian: `float32 x, y, size, angle`; `int32 shape`; `float32 r, g, b, a`. The instance count is the blob size / 36. This is the literal sim output, for a consumer that wants to draw the instances itself.
- **`poxicle_engine_tick_vertices(dt)`** returns the frame already **expanded to a GPU-ready vertex blob** — an interleaved `CoglVertexP2C4` triangle list (`float x, y` then 4 *premultiplied* `u8 r, g, b, a`, a 12-byte stride). The vertex count is the blob size / 12.

The GNOME extension uses `tick_vertices` exclusively, because the alternative — a per-frame, window-sized CPU surface drawn with Cairo — stalled interactive resize. Triangulating in C and handing gjs a blob it can upload straight to a `Cogl.AttributeBuffer` removes Cairo and the per-frame surface entirely.

## 4. GPU-ready triangulation

`instances_to_vertices()` is the shared expander that both `tick_vertices` and the stream reader call, so local-sim and streamed frames emit byte-identical geometry. It triangulates each shape in C:

| Shape | Triangulation |
| --- | --- |
| square / diamond | two triangles |
| triangle (rotated) | one triangle, rotated by the instance angle |
| circle | a fan of `CIRCLE_SEGS = 12` triangles |

Each vertex is pushed by `push_vert()` as 12 bytes: `float x, y`, then four `u8` colour components **premultiplied** by alpha (`rgb * a * 255`). This matches the [renderer's](renderer.gen.html) premultiplied output and the Cogl "over" blend the [extension](gnome-extension.gen.html) sets. Invisible particles (`color.a <= 0.003`) are dropped during expansion.

## 5. Config-driven look, resolved in C

The GNOME extension and the KWin effect read the *same* DE-neutral config file, `~/.config/poxicle/poxicle.conf`. Rather than maintain a second full parser in gjs, the binding resolves and applies the whole look in C, mirroring the KWin effect's `poxconfig` field-for-field. Two entry points do this:

- **`poxicle_engine_apply_config(wm_class)`** resolves the complete look for a focused window's WM class — its preset, the stored `Preset-<name>` parameter edits, the per-app override columns, palette and reverse — in one call. The first per-app rule whose appId is a case-insensitive substring of `wm_class` wins; otherwise the focus-following `Active` target applies. It also sets global corner rounding from the `CornerTop` / `CornerBottom` keys. With no config file yet, it applies the out-of-box ambient look. Returns `FALSE` when the window should draw nothing (no matching rule, preset `none`, or an unset target).
- **`poxicle_engine_apply_panel_config()`** resolves the desktop-panel `Panel` target — packed exactly like `Active`. Unlike `apply_config` it deliberately does **not** read the corner keys: the panel keeps sharp corners. The host sets which edges face the screen interior via `set_edge_mask`.

The full schema these read — the `[poxicle]` group, the `Rules` / `Active` / `Panel` packing, the percent-unit override columns, the palette precedence — is documented once in [Configuration & Presets](configuration.gen.html). Keeping the resolver in C means every backend honours the same edits with one implementation; the [GNOME extension](gnome-extension.gen.html) only *watches* the file and calls these two methods.

## 6. The stream receiver

The binding is also the GNOME side of the [poxbridge protocol](poxbridge.gen.html): it can draw a producer's streamed frames instead of running a local sim, so the extension can own `org.ninez.PoxicleBridge` just like the KWin effect.

| Method | Role |
| --- | --- |
| `poxicle_engine_attach_stream(fd)` | `mmap`s a producer's memfd read-only, validates the `PoxBridgeHeader` (magic, version, `inst_size`, size), and takes ownership of the fd (closes it; the mmap outlives it). |
| `poxicle_engine_read_stream_vertices()` | Reads the latest *complete* frame via the seqlock and expands it to the same vertex blob as `tick_vertices`. |
| `poxicle_engine_detach_stream()` | Unmaps; called automatically on finalize and explicitly when the producer unregisters. |
| `poxicle_engine_stream_width/height()` | The producer's surface size, for placing the overlay. |

`read_stream_vertices` returns `NULL` when no new frame has arrived since the last call (the caller keeps drawing its last blob) and a zero-length blob when a new frame has no live particles (the caller clears). Its seqlock reader retries up to eight times with acquire-ordered atomic loads, skipping odd (mid-write) and unchanged (no new frame) sequences and re-reading after the copy to reject torn frames — the exact discipline the [protocol](poxbridge.gen.html) defines.

## 7. Catalogue helpers

Four namespace-level functions wrap the engine's canonical tables so a binding can list and preview them without an engine instance: `poxicle_preset_count()` / `poxicle_preset_name(id)` and `poxicle_palette_count()` / `poxicle_palette_name(id)`. They forward straight to the engine's `pox_preset_*` / `pox_palette_*` lookups, so a UI that lists presets or palettes always reflects exactly what the engine renders.

## 8. Design rules

- **One engine, many languages.** The binding adds a GObject layer and nothing else; the core stays dependency-free, and a gjs or Python consumer drives the same C engine the compositor effects link.
- **Hand the GPU a blob.** `tick_vertices` triangulates in C and returns interleaved, premultiplied `CoglVertexP2C4` vertices, so a binding uploads and draws in one primitive — no Cairo, no per-frame surface.
- **Resolve the look in C.** `apply_config` / `apply_panel_config` parse the shared neutral config once in C, so every backend honours the same edits without a second parser.
- **One expander for both paths.** Local-sim and streamed frames go through `instances_to_vertices`, guaranteeing byte-identical geometry.
- **Be the GNOME bridge receiver too.** The same `attach_stream` / `read_stream_vertices` seqlock reader the KWin effect uses lets the extension own `org.ninez.PoxicleBridge`.
